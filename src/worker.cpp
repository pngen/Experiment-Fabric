// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "experiment_fabric/worker.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <random>
#include <utility>

#include "experiment_fabric/hash.hpp"
#include "experiment_fabric/version.hpp"

namespace experiment_fabric {
namespace {

/// Mints a fresh process-incarnation identity. Randomness is combined with the
/// process clock so two incarnations started in the same tick still differ.
WorkerBootId mint_boot_identity() {
  std::random_device device;
  std::uint64_t value = (static_cast<std::uint64_t>(device()) << 32) ^ static_cast<std::uint64_t>(device());
  value ^= static_cast<std::uint64_t>(
      std::chrono::high_resolution_clock::now().time_since_epoch().count());
  value ^= static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
  value = (value == 0) ? 1 : value;
  return WorkerBootId::from_value(value);
}

}  // namespace

struct Worker::Impl {
  Impl(Config config_in, Limits limits_in) : config(std::move(config_in)), limits(limits_in) {}

  Config config;
  Limits limits;
  std::unique_ptr<Client> client;
  WorkerBootId boot;
  CoordinatorEpoch epoch;
  ProducerId producer;
  std::uint32_t published = 0;
  std::uint32_t failed = 0;
  std::string last_error;
  std::ofstream frame_log;
  std::uint64_t published_observations = 0;

  void record_frame(std::span<const std::uint8_t> bytes) {
    if (!frame_log.is_open()) {
      return;
    }
    const std::uint64_t length = bytes.size();
    frame_log.write(reinterpret_cast<const char*>(&length), sizeof(length));
    frame_log.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    frame_log.flush();
  }
};

Worker::Worker(Config config) : impl_(std::make_unique<Impl>(std::move(config), Limits{})) {
  impl_->limits = impl_->config.limits;
}

Worker::~Worker() = default;

Result<std::unique_ptr<Worker>> Worker::create(Config config) {
  if (!config.worker.valid()) {
    return make_error(ErrorCode::INVALID_ARGUMENT, ErrorStage::IDENTITY, "worker identity must be non-null");
  }
  std::unique_ptr<Worker> worker(new Worker(std::move(config)));
  Impl& impl = *worker->impl_;
  impl.boot = impl.config.forced_boot.has_value() ? *impl.config.forced_boot : mint_boot_identity();
  if (!impl.boot.valid()) {
    return make_error(ErrorCode::INVALID_ARGUMENT, ErrorStage::IDENTITY, "worker boot identity must be non-null");
  }
  if (!impl.config.frame_log_path.empty()) {
    impl.frame_log.open(impl.config.frame_log_path, std::ios::binary | std::ios::trunc);
    if (!impl.frame_log.is_open()) {
      return make_error(ErrorCode::PERSISTENCE_FAILURE, ErrorStage::TRANSPORT,
                        "cannot open the worker frame log for writing");
    }
  }
  Client::Config client_config;
  client_config.host = impl.config.host;
  client_config.port = impl.config.port;
  client_config.limits = impl.limits;
  if (!impl.config.frame_log_path.empty()) {
    client_config.outbound_observer = [&impl](std::span<const std::uint8_t> bytes) {
      impl.record_frame(bytes);
    };
  }
  auto client = Client::connect(std::move(client_config));
  if (!client.ok()) {
    return client.status();
  }
  impl.client = std::move(client.value());
  return worker;
}

Status Worker::connect_and_register() {
  Impl& impl = *impl_;
  RegisterWorkerRequest request;
  request.claimed_epoch = CoordinatorEpoch{};
  request.worker = impl.config.worker;
  request.boot = impl.boot;
  request.endpoint = impl.config.endpoint;
  request.max_concurrent_assignments = impl.config.max_concurrent_assignments;
  request.fingerprint = impl.config.fingerprint;
  auto reply = impl.client->register_worker(request);
  if (!reply.ok()) {
    impl.last_error = reply.status().to_string();
    return reply.status();
  }
  if (reply.value().status.failed()) {
    impl.last_error = reply.value().status.to_string();
    return reply.value().status;
  }
  impl.epoch = reply.value().epoch;
  impl.producer = reply.value().producer;
  impl.boot = reply.value().boot;
  return Status::success();
}

Status Worker::run_once(const TaskFunction& task) {
  Impl& impl = *impl_;
  ClaimTrialRequest request;
  request.epoch = impl.epoch;
  request.worker = impl.config.worker;
  request.boot = impl.boot;
  request.max_claims = impl.config.max_concurrent_assignments;

  auto reply = impl.client->claim_trials(request);
  if (!reply.ok()) {
    impl.last_error = reply.status().to_string();
    return reply.status();
  }
  if (reply.value().status.failed()) {
    impl.last_error = reply.value().status.to_string();
    return reply.value().status;
  }
  if (reply.value().assignments.empty()) {
    return make_error(ErrorCode::NOT_FOUND, ErrorStage::LIFECYCLE, "no trial is available to claim");
  }

  for (const TrialAssignment& assignment : reply.value().assignments) {
    WorkerTaskContext context;
    context.worker = impl.config.worker;
    context.boot = impl.boot;
    context.epoch = impl.epoch;
    context.producer = impl.producer;
    context.attempt = assignment.attempt_number;
    context.parameters = assignment.parameters;

    TrialOutcome outcome = task(assignment, context);
    if (!outcome.succeeded) {
      FailTrialRequest failure;
      failure.epoch = impl.epoch;
      failure.worker = impl.config.worker;
      failure.boot = impl.boot;
      failure.envelope = assignment.envelope;
      failure.kind = outcome.failure_kind;
      failure.detail = outcome.failure_detail;
      failure.retryable = outcome.retryable;
      const Status status = impl.client->fail_trial(failure);
      if (status.ok()) {
        ++impl.failed;
      } else {
        impl.last_error = status.to_string();
      }
      continue;
    }

    bool published_all = true;
    for (const NamedObservation& named : outcome.observations) {
      const AssignedMetric* reservation = nullptr;
      for (const AssignedMetric& candidate : assignment.metrics) {
        if (candidate.name == named.metric) {
          reservation = &candidate;
        }
      }
      if (reservation == nullptr) {
        impl.last_error = "task produced an observation for a metric that was not assigned: " + named.metric;
        published_all = false;
        break;
      }
      PublishObservationRequest publish;
      publish.epoch = impl.epoch;
      publish.worker = impl.config.worker;
      publish.boot = impl.boot;
      publish.observation.id = reservation->observation_id;
      publish.observation.experiment = assignment.envelope.experiment;
      publish.observation.generation = assignment.envelope.generation;
      publish.observation.branch = assignment.envelope.branch;
      publish.observation.branch_generation = assignment.envelope.branch_generation;
      publish.observation.trial = assignment.envelope.trial;
      publish.observation.attempt = assignment.envelope.attempt;
      publish.observation.metric = reservation->metric;
      publish.observation.producer = impl.producer;
      publish.observation.worker = impl.config.worker;
      publish.observation.boot = impl.boot;
      publish.observation.epoch = impl.epoch;
      publish.observation.validity = named.validity;
      publish.observation.value = named.value;
      publish.observation.detail = named.detail;
      const Status status = impl.client->publish_observation(publish);
      if (!status.ok()) {
        impl.last_error = status.to_string();
        published_all = false;
        break;
      }
      ++impl.published_observations;
      if (impl.config.progress_hook) {
        impl.config.progress_hook(impl.published_observations, impl.published);
      }
    }

    if (!published_all) {
      FailTrialRequest failure;
      failure.epoch = impl.epoch;
      failure.worker = impl.config.worker;
      failure.boot = impl.boot;
      failure.envelope = assignment.envelope;
      failure.kind = FailureKind::INVALID_OBSERVATION;
      failure.detail = impl.last_error;
      failure.retryable = false;
      (void)impl.client->fail_trial(failure);
      ++impl.failed;
      continue;
    }

    CommitTrialRequest commit;
    commit.epoch = impl.epoch;
    commit.worker = impl.config.worker;
    commit.boot = impl.boot;
    commit.envelope = assignment.envelope;
    commit.result_payload = outcome.result_payload;
    std::size_t artifact_index = 0;
    for (const NamedArtifact& named : outcome.artifacts) {
      if (artifact_index >= assignment.artifact_ids.size()) {
        break;
      }
      ArtifactRecord artifact;
      artifact.id = assignment.artifact_ids[artifact_index++];
      artifact.experiment = assignment.envelope.experiment;
      artifact.generation = assignment.envelope.generation;
      artifact.branch = assignment.envelope.branch;
      artifact.trial = assignment.envelope.trial;
      artifact.attempt = assignment.envelope.attempt;
      artifact.category = named.category;
      artifact.locator = named.locator;
      artifact.content_digest = named.content_digest;
      artifact.digest_present = named.digest_present;
      artifact.size_present = named.size_present;
      artifact.size_bytes = named.size_bytes;
      artifact.provenance = named.provenance;
      commit.artifacts.push_back(std::move(artifact));
    }
    const Status status = impl.client->commit_trial(commit);
    if (!status.ok()) {
      impl.last_error = status.to_string();
      ++impl.failed;
    } else {
      ++impl.published;
    }
  }
  return Status::success();
}

Status Worker::run_until_idle(const TaskFunction& task, const std::atomic<bool>& stop) {
  Impl& impl = *impl_;
  while (!stop.load()) {
    if (!impl.epoch.valid()) {
      const Status registered = connect_and_register();
      if (!registered.ok()) {
        return registered;
      }
    }
    const Status status = run_once(task);
    if (status.ok()) {
      continue;
    }
    if (status.code() == ErrorCode::NOT_FOUND) {
      return Status::success();
    }
    if (status.code() == ErrorCode::STALE_EPOCH || status.code() == ErrorCode::STALE_WORKER) {
      // A restarted coordinator or a replaced incarnation requires a fresh
      // registration before any further work can be claimed.
      const Status registered = connect_and_register();
      if (!registered.ok()) {
        return registered;
      }
      continue;
    }
    return status;
  }
  return Status::success();
}

WorkerBootId Worker::boot() const noexcept { return impl_->boot; }

CoordinatorEpoch Worker::epoch() const noexcept { return impl_->epoch; }

ProducerId Worker::producer() const noexcept { return impl_->producer; }

std::uint32_t Worker::published_trials() const noexcept { return impl_->published; }

std::uint32_t Worker::failed_trials() const noexcept { return impl_->failed; }

const std::string& Worker::last_error() const noexcept { return impl_->last_error; }

const std::filesystem::path& Worker::frame_log_path() const noexcept { return impl_->config.frame_log_path; }

Result<Frame> Worker::replay_raw(std::span<const std::uint8_t> bytes) {
  return impl_->client->send_raw_and_receive(bytes);
}

Status Worker::send_raw(std::span<const std::uint8_t> bytes) { return impl_->client->send_raw(bytes); }

const std::string* WorkerTaskContext::parameter(std::string_view key) const noexcept {
  for (const ParameterAssignment& assignment : parameters) {
    if (assignment.key == key) {
      return &assignment.value;
    }
  }
  return nullptr;
}

Client& Worker::client() { return *impl_->client; }

}  // namespace experiment_fabric
