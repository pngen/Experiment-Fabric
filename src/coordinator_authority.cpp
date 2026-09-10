// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

#include "coordinator_internal.hpp"

namespace experiment_fabric {
namespace {

Status authority_error(ErrorCode code, std::string reason) {
  return make_error(code, ErrorStage::AUTHORITY, std::move(reason));
}

Status limits_error(std::string reason) {
  return make_error(ErrorCode::RESOURCE_EXHAUSTED, ErrorStage::LIMITS, std::move(reason));
}

Status observation_error(std::string reason) {
  return make_error(ErrorCode::INVALID_OBSERVATION, ErrorStage::OBSERVATION, std::move(reason));
}

Status protocol_error(std::string reason) {
  return make_error(ErrorCode::PROTOCOL_ERROR, ErrorStage::TRANSPORT, std::move(reason));
}

AuthorityVerdict reject(ErrorCode code, std::string detail) {
  return AuthorityVerdict::reject(code, ErrorStage::AUTHORITY, std::move(detail));
}

bool same_metric_value(const MetricValue& lhs, const MetricValue& rhs) {
  return lhs.kind == rhs.kind && lhs.real == rhs.real && lhs.integer == rhs.integer &&
         lhs.boolean == rhs.boolean && lhs.category == rhs.category;
}

bool same_observation(const Observation& lhs, const Observation& rhs) {
  return lhs.id == rhs.id && lhs.experiment == rhs.experiment && lhs.generation == rhs.generation &&
         lhs.branch == rhs.branch && lhs.branch_generation == rhs.branch_generation && lhs.trial == rhs.trial &&
         lhs.attempt == rhs.attempt && lhs.metric == rhs.metric && lhs.producer == rhs.producer &&
         lhs.validity == rhs.validity && same_metric_value(lhs.value, rhs.value) && lhs.detail == rhs.detail;
}

bool same_artifact(const ArtifactRecord& lhs, const ArtifactRecord& rhs) {
  return lhs.id == rhs.id && lhs.experiment == rhs.experiment && lhs.generation == rhs.generation &&
         lhs.branch == rhs.branch && lhs.trial == rhs.trial && lhs.attempt == rhs.attempt &&
         lhs.category == rhs.category && lhs.locator == rhs.locator &&
         lhs.content_digest == rhs.content_digest && lhs.digest_present == rhs.digest_present &&
         lhs.size_present == rhs.size_present && lhs.size_bytes == rhs.size_bytes &&
         lhs.provenance == rhs.provenance;
}

std::string describe_epoch(CoordinatorEpoch expected, CoordinatorEpoch actual) {
  std::string detail = "stale coordinator epoch: envelope=";
  detail.append(actual.to_string());
  detail.append(" current=");
  detail.append(expected.to_string());
  return detail;
}

/// Builds the envelope presented by an observation or artifact publication.
AuthorityEnvelope envelope_from(const Observation& observation, CoordinatorEpoch epoch, WorkerId worker,
                                WorkerBootId boot, const ExperimentRecord& record) {
  AuthorityEnvelope envelope;
  envelope.epoch = epoch;
  envelope.worker = worker;
  envelope.boot = boot;
  envelope.experiment = observation.experiment;
  envelope.generation = observation.generation;
  envelope.hypothesis = record.definition.hypothesis;
  envelope.hypothesis_revision = record.definition.hypothesis_revision;
  envelope.policy = record.definition.policy.id;
  envelope.policy_generation = record.definition.policy.generation;
  envelope.branch = observation.branch;
  envelope.branch_generation = observation.branch_generation;
  envelope.trial = observation.trial;
  envelope.attempt = observation.attempt;
  envelope.producer = observation.producer;
  return envelope;
}

}  // namespace

EnvelopeCheck Coordinator::Impl::check_envelope(const AuthorityEnvelope& envelope) {
  EnvelopeCheck check;
  const AuthorityVerdict structural = validate_envelope_structure(envelope);
  if (!structural.accepted) {
    check.verdict = structural;
    return check;
  }
  if (!admitting_work) {
    check.verdict = reject(ErrorCode::SHUTTING_DOWN, "coordinator is no longer admitting authority");
    return check;
  }
  if (envelope.epoch != epoch) {
    check.verdict = reject(ErrorCode::STALE_EPOCH, describe_epoch(epoch, envelope.epoch));
    return check;
  }
  LiveWorker* live = find_live_worker(envelope.worker, envelope.boot);
  if (live == nullptr) {
    check.verdict = reject(ErrorCode::STALE_WORKER, "worker boot identity is not registered under the current epoch");
    return check;
  }
  if (live->revoked) {
    check.verdict = reject(ErrorCode::STALE_WORKER, "worker incarnation authority has been revoked");
    return check;
  }
  if (live->producer != envelope.producer) {
    check.verdict = reject(ErrorCode::UNAUTHORIZED, "producer identity does not match the registered incarnation");
    return check;
  }
  ExperimentRecord* record = find_experiment(envelope.experiment);
  if (record == nullptr) {
    check.verdict = reject(ErrorCode::NOT_FOUND, "experiment does not exist");
    return check;
  }
  check.record = record;
  if (record->state == ExperimentState::CANCELLED) {
    check.verdict = reject(ErrorCode::CANCELLED, "experiment was cancelled: late evidence is never current authority");
    return check;
  }
  if (record->state == ExperimentState::SUPERSEDED) {
    check.verdict = reject(ErrorCode::STALE_GENERATION, "experiment has been superseded");
    return check;
  }
  if (!is_experiment_mutable(record->state)) {
    check.verdict = reject(ErrorCode::INVALID_TRANSITION, "experiment no longer accepts authoritative evidence");
    return check;
  }
  if (envelope.generation != record->definition.generation) {
    std::string detail = "stale experiment generation: envelope=";
    detail.append(envelope.generation.to_string());
    detail.append(" current=");
    detail.append(record->definition.generation.to_string());
    check.verdict = reject(ErrorCode::STALE_GENERATION, std::move(detail));
    return check;
  }
  if (envelope.hypothesis != record->definition.hypothesis ||
      envelope.hypothesis_revision != record->definition.hypothesis_revision) {
    check.verdict = reject(ErrorCode::STALE_HYPOTHESIS, "stale hypothesis revision");
    return check;
  }
  if (envelope.policy != record->definition.policy.id ||
      envelope.policy_generation != record->definition.policy.generation) {
    check.verdict = reject(ErrorCode::STALE_GENERATION, "stale comparison policy identity or generation");
    return check;
  }
  BranchDefinition* branch = find_branch(*record, envelope.branch);
  if (branch == nullptr) {
    check.verdict = reject(ErrorCode::STALE_BRANCH, "branch does not exist in the current experiment generation");
    return check;
  }
  check.branch = branch;
  if (branch->generation != envelope.branch_generation) {
    std::string detail = "stale branch generation: envelope=";
    detail.append(envelope.branch_generation.to_string());
    detail.append(" current=");
    detail.append(branch->generation.to_string());
    check.verdict = reject(ErrorCode::STALE_BRANCH, std::move(detail));
    return check;
  }
  if (!is_branch_eligible(branch->state)) {
    check.verdict = reject(ErrorCode::STALE_BRANCH, std::string("branch is not eligible for evidence: ") +
                                                        std::string(to_string(branch->state)));
    return check;
  }
  const auto trial_iterator = record->trials.find(envelope.trial);
  if (trial_iterator == record->trials.end()) {
    check.verdict = reject(ErrorCode::STALE_ATTEMPT, "trial does not exist in the current experiment generation");
    return check;
  }
  TrialRecord* trial = &trial_iterator->second;
  check.trial = trial;
  if (trial->branch != envelope.branch) {
    check.verdict = reject(ErrorCode::STALE_BRANCH, "trial does not belong to the presented branch");
    return check;
  }
  if (trial->generation != envelope.generation) {
    check.verdict = reject(ErrorCode::STALE_GENERATION, "trial was created under a different experiment generation");
    return check;
  }
  if (trial->branch_generation != envelope.branch_generation) {
    check.verdict = reject(ErrorCode::STALE_BRANCH, "trial was created under a different branch generation");
    return check;
  }
  if (trial->hypothesis_revision != envelope.hypothesis_revision) {
    check.verdict = reject(ErrorCode::STALE_HYPOTHESIS, "trial was created under a different hypothesis revision");
    return check;
  }
  if (trial->policy != envelope.policy || trial->policy_generation != envelope.policy_generation) {
    check.verdict = reject(ErrorCode::STALE_GENERATION, "trial was created under a different comparison policy");
    return check;
  }
  if (trial->state == TrialState::CANCELLED) {
    check.verdict = reject(ErrorCode::CANCELLED, "trial was cancelled: late evidence is never current authority");
    return check;
  }
  if (is_terminal(trial->state)) {
    check.verdict = reject(ErrorCode::STALE_ATTEMPT,
                           std::string("trial is already terminal: ") + std::string(to_string(trial->state)));
    return check;
  }
  if (trial->authoritative_attempt != envelope.attempt) {
    check.verdict = reject(ErrorCode::STALE_ATTEMPT, "attempt is not the authoritative attempt of this trial");
    return check;
  }
  const auto attempt_iterator = record->attempts.find(envelope.attempt);
  if (attempt_iterator == record->attempts.end()) {
    check.verdict = reject(ErrorCode::STALE_ATTEMPT, "attempt identity is not recorded");
    return check;
  }
  AttemptRecord* attempt = &attempt_iterator->second;
  check.attempt = attempt;
  if (attempt->worker != envelope.worker || attempt->boot != envelope.boot) {
    check.verdict = reject(ErrorCode::STALE_WORKER, "attempt was issued to a different worker incarnation");
    return check;
  }
  if (attempt->epoch != envelope.epoch) {
    check.verdict = reject(ErrorCode::STALE_EPOCH, "attempt was issued under a different coordinator epoch");
    return check;
  }
  if (is_attempt_terminal(attempt->state)) {
    check.verdict = reject(ErrorCode::STALE_ATTEMPT,
                           std::string("attempt is already terminal: ") + std::string(to_string(attempt->state)));
    return check;
  }
  check.verdict = AuthorityVerdict::accept();
  return check;
}

// ---------------------------------------------------------------------------
// Worker lifecycle
// ---------------------------------------------------------------------------

Result<RegisterWorkerReply> Coordinator::register_worker(const RegisterWorkerRequest& request) {
  Impl& impl = *impl_;
  RegisterWorkerReply reply;
  {
    std::unique_lock<std::shared_mutex> lock(impl.mutex);
    if (!impl.admitting_work) {
      reply.status = make_error(ErrorCode::SHUTTING_DOWN, ErrorStage::SHUTDOWN, "coordinator is shutting down");
      return reply;
    }
    if (!request.worker.valid() || !request.boot.valid()) {
      reply.status = authority_error(ErrorCode::INVALID_ARGUMENT, "worker and boot identities must be non-null");
      return reply;
    }
    if (request.claimed_epoch.valid() && request.claimed_epoch != impl.epoch) {
      reply.status = authority_error(ErrorCode::STALE_EPOCH, describe_epoch(impl.epoch, request.claimed_epoch));
      return reply;
    }
    if (request.max_concurrent_assignments == 0 ||
        request.max_concurrent_assignments > impl.limits.max_concurrent_assignments_per_worker) {
      reply.status = limits_error("worker concurrency request is out of range");
      return reply;
    }
    const auto existing = impl.live_workers.find({request.worker.value(), request.boot.value()});
    if (existing != impl.live_workers.end()) {
      if (existing->second.revoked) {
        reply.status = authority_error(ErrorCode::STALE_WORKER, "worker incarnation was revoked");
        return reply;
      }
      reply.epoch = impl.epoch;
      reply.boot = request.boot;
      reply.producer = existing->second.producer;
      reply.accepted = true;
      reply.status = Status::success();
      return reply;
    }
    // A different boot identity for the same logical worker means the previous
    // process incarnation is gone. Its live authority is revoked and its
    // unfinished assignments are transitioned conservatively before the new
    // incarnation is accepted, so old traffic can never be accepted afterwards.
    const std::uint64_t sequence = impl.next_sequence();
    for (auto& entry : impl.live_workers) {
      LiveWorker& live = entry.second;
      if (live.worker != request.worker || live.boot == request.boot || live.revoked) {
        continue;
      }
      live.revoked = true;
      live.revocation_kind = FailureKind::PRODUCER_LOSS;
      for (WorkerIncarnation& incarnation : impl.state.workers) {
        if (incarnation.worker == live.worker && incarnation.boot == live.boot && !incarnation.revoked) {
          incarnation.revoked = true;
          incarnation.revocation_kind = FailureKind::PRODUCER_LOSS;
          incarnation.revoked_sequence = sequence;
        }
      }
      for (auto& experiment_entry : impl.state.experiments) {
        ExperimentRecord& record = experiment_entry.second;
        bool touched = false;
        for (auto& attempt_entry : record.attempts) {
          AttemptRecord& attempt = attempt_entry.second;
          if (attempt.worker != live.worker || attempt.boot != live.boot || is_attempt_terminal(attempt.state)) {
            continue;
          }
          attempt.state = AttemptState::ABANDONED;
          attempt.failure_kind = FailureKind::PRODUCER_LOSS;
          attempt.failure_detail = "worker process incarnation replaced by a fresh boot identity";
          attempt.updated_sequence = sequence;
          touched = true;
          const auto trial_iterator = record.trials.find(attempt.trial);
          if (trial_iterator == record.trials.end()) {
            continue;
          }
          TrialRecord& trial = trial_iterator->second;
          if (is_terminal(trial.state) || trial.authoritative_attempt != attempt.id) {
            continue;
          }
          const bool retryable = record.definition.retry_policy != RetryPolicy::NO_RETRY &&
                                 trial.attempt_count < record.definition.max_attempts_per_trial;
          if (retryable) {
            trial.state = TrialState::READY;
            trial.authoritative_attempt = TrialAttemptId{};
            trial.next_attempt_number = TrialAttemptNumber::from_value(trial.attempt_count + 1);
          } else {
            trial.state = TrialState::FAILED;
          }
          trial.failure_kind = FailureKind::PRODUCER_LOSS;
          trial.failure_detail = "producer lost; assignment transitioned conservatively";
          trial.updated_sequence = sequence;
        }
        if (touched) {
          impl.log(record, "worker-incarnation-replaced worker=" + live.worker.to_string());
        }
      }
    }
    for (const WorkerIncarnation& incarnation : impl.state.workers) {
      if (incarnation.worker != request.worker || incarnation.boot != request.boot) {
        continue;
      }
      if (incarnation.revoked) {
        reply.status = authority_error(ErrorCode::STALE_WORKER, "worker incarnation was previously revoked");
        return reply;
      }
      if (incarnation.epoch != impl.epoch) {
        // A process-incarnation identity belongs to the coordinator epoch that
        // issued it. Replaying it under a later epoch must never resurrect the
        // authority of a coordinator that no longer exists.
        reply.status = authority_error(ErrorCode::STALE_EPOCH,
                                       "worker boot identity belongs to a previous coordinator epoch");
        return reply;
      }
    }
    if (impl.state.workers.size() >= impl.limits.max_registered_workers) {
      reply.status = limits_error("too many registered worker incarnations");
      return reply;
    }

    LiveWorker live;
    live.worker = request.worker;
    live.boot = request.boot;
    live.endpoint = request.endpoint;
    live.fingerprint = request.fingerprint;
    live.max_concurrent_assignments = request.max_concurrent_assignments;
    live.registered_sequence = impl.next_sequence();
    live.producer = ProducerId::from_value(++impl.state.next_worker_registration_sequence);
    impl.live_workers.emplace(std::make_pair(request.worker.value(), request.boot.value()), live);

    WorkerIncarnation incarnation;
    incarnation.worker = live.worker;
    incarnation.boot = live.boot;
    incarnation.epoch = impl.epoch;
    incarnation.producer = live.producer;
    incarnation.endpoint = live.endpoint;
    incarnation.fingerprint = live.fingerprint;
    incarnation.registered_sequence = live.registered_sequence;
    impl.state.workers.push_back(incarnation);

    const Status persisted = impl.persist_locked();
    if (!persisted.ok()) {
      reply.status = persisted;
      return reply;
    }
    reply.epoch = impl.epoch;
    reply.boot = live.boot;
    reply.producer = live.producer;
    reply.accepted = true;
    reply.status = Status::success();
  }
  impl.notify_event("worker-registered:" + request.worker.to_string());
  return reply;
}

Status Coordinator::revoke_worker(WorkerId worker, WorkerBootId boot, FailureKind kind, std::string reason) {
  Impl& impl = *impl_;
  {
    std::unique_lock<std::shared_mutex> lock(impl.mutex);
    LiveWorker* live = impl.find_live_worker(worker, boot);
    if (live == nullptr) {
      return authority_error(ErrorCode::STALE_WORKER, "worker incarnation is not registered");
    }
    live->revoked = true;
    live->revocation_kind = kind;
    const std::uint64_t sequence = impl.next_sequence();
    for (auto& entry : impl.state.experiments) {
      ExperimentRecord& record = entry.second;
      bool touched = false;
      for (auto& attempt_entry : record.attempts) {
        AttemptRecord& attempt = attempt_entry.second;
        if (attempt.worker != worker || attempt.boot != boot || is_attempt_terminal(attempt.state)) {
          continue;
        }
        attempt.state = AttemptState::ABANDONED;
        attempt.failure_kind = kind;
        attempt.failure_detail = reason;
        attempt.updated_sequence = sequence;
        touched = true;
        const auto trial_iterator = record.trials.find(attempt.trial);
        if (trial_iterator == record.trials.end()) {
          continue;
        }
        TrialRecord& trial = trial_iterator->second;
        if (is_terminal(trial.state) || trial.authoritative_attempt != attempt.id) {
          continue;
        }
        const bool retryable = record.definition.retry_policy != RetryPolicy::NO_RETRY &&
                               trial.attempt_count < record.definition.max_attempts_per_trial;
        if (retryable) {
          trial.state = TrialState::READY;
          trial.authoritative_attempt = TrialAttemptId{};
          trial.next_attempt_number = TrialAttemptNumber::from_value(trial.attempt_count + 1);
        } else {
          trial.state = TrialState::FAILED;
        }
        trial.failure_kind = kind;
        trial.failure_detail = reason;
        trial.updated_sequence = sequence;
      }
      if (touched) {
        impl.log(record, "worker-revoked worker=" + worker.to_string() + " reason=" + reason);
      }
    }
    for (WorkerIncarnation& incarnation : impl.state.workers) {
      if (incarnation.worker == worker && incarnation.boot == boot && !incarnation.revoked) {
        incarnation.revoked = true;
        incarnation.revocation_kind = kind;
        incarnation.revoked_sequence = sequence;
      }
    }
    const Status persisted = impl.persist_locked();
    if (!persisted.ok()) {
      return persisted;
    }
  }
  impl.notify_event("worker-revoked:" + worker.to_string());
  return Status::success();
}

Status Coordinator::heartbeat(const HeartbeatRequest& request) {
  Impl& impl = *impl_;
  std::unique_lock<std::shared_mutex> lock(impl.mutex);
  if (!impl.admitting_work) {
    return make_error(ErrorCode::SHUTTING_DOWN, ErrorStage::SHUTDOWN, "coordinator is shutting down");
  }
  if (request.epoch != impl.epoch) {
    return authority_error(ErrorCode::STALE_EPOCH, describe_epoch(impl.epoch, request.epoch));
  }
  LiveWorker* live = impl.find_live_worker(request.worker, request.boot);
  if (live == nullptr || live->revoked) {
    return authority_error(ErrorCode::STALE_WORKER, "worker incarnation is not live under the current epoch");
  }
  live->active_assignments = request.active_assignments;
  return Status::success();
}

Result<ClaimTrialReply> Coordinator::claim_trials(const ClaimTrialRequest& request) {
  Impl& impl = *impl_;
  ClaimTrialReply reply;
  {
    std::unique_lock<std::shared_mutex> lock(impl.mutex);
    if (!impl.admitting_work) {
      reply.status = make_error(ErrorCode::SHUTTING_DOWN, ErrorStage::SHUTDOWN, "coordinator is shutting down");
      return reply;
    }
    if (request.epoch != impl.epoch) {
      reply.status = authority_error(ErrorCode::STALE_EPOCH, describe_epoch(impl.epoch, request.epoch));
      return reply;
    }
    LiveWorker* live = impl.find_live_worker(request.worker, request.boot);
    if (live == nullptr || live->revoked) {
      reply.status =
          authority_error(ErrorCode::STALE_WORKER, "worker incarnation is not live under the current epoch");
      return reply;
    }
    if (request.max_claims == 0 || request.max_claims > impl.limits.max_batch_items) {
      reply.status = limits_error("claim batch size is out of range");
      return reply;
    }
    // Concurrency head-room is derived from the durable attempt records rather
    // than from a counter, so it stays correct across retries, cancellations,
    // worker loss and coordinator recovery without any bookkeeping to drift.
    std::uint32_t active = 0;
    for (const auto& experiment_entry : impl.state.experiments) {
      for (const auto& attempt_entry : experiment_entry.second.attempts) {
        const AttemptRecord& tracked = attempt_entry.second;
        if (tracked.worker == request.worker && tracked.boot == request.boot &&
            !is_attempt_terminal(tracked.state)) {
          ++active;
        }
      }
    }
    live->active_assignments = active;
    const std::uint32_t available =
        live->max_concurrent_assignments > active ? live->max_concurrent_assignments - active : 0;
    std::uint32_t budget = std::min(request.max_claims, available);
    for (auto& experiment_entry : impl.state.experiments) {
      if (budget == 0) {
        break;
      }
      ExperimentRecord& record = experiment_entry.second;
      if (!is_experiment_mutable(record.state)) {
        continue;
      }
      for (auto& trial_entry : record.trials) {
        if (budget == 0) {
          break;
        }
        TrialRecord& trial = trial_entry.second;
        if (trial.state != TrialState::READY || trial.generation != record.definition.generation ||
            trial.hypothesis_revision != record.definition.hypothesis_revision) {
          continue;
        }
        if (trial.attempt_count >= record.definition.max_attempts_per_trial) {
          continue;
        }
        if (record.definition.retry_policy == RetryPolicy::NO_RETRY && trial.attempt_count > 0) {
          continue;
        }
        BranchDefinition* branch = impl.find_branch(record, trial.branch);
        if (branch == nullptr || !is_branch_eligible(branch->state) ||
            branch->generation != trial.branch_generation) {
          continue;
        }
        if (record.attempts.size() >= record.trials.size() * static_cast<std::size_t>(
                                                               impl.limits.max_attempts_per_trial) +
                                         16) {
          continue;
        }

        const std::uint64_t sequence = impl.next_sequence();
        AttemptRecord attempt;
        attempt.id = impl.alloc_attempt_id();
        attempt.number = trial.next_attempt_number.valid()
                             ? trial.next_attempt_number
                             : TrialAttemptNumber::from_value(trial.attempt_count + 1);
        attempt.trial = trial.id;
        attempt.experiment = record.definition.id;
        attempt.generation = record.definition.generation;
        attempt.branch = trial.branch;
        attempt.branch_generation = trial.branch_generation;
        attempt.worker = request.worker;
        attempt.boot = request.boot;
        attempt.epoch = impl.epoch;
        attempt.producer = live->producer;
        attempt.state = AttemptState::ISSUED;
        attempt.issued_sequence = sequence;
        attempt.updated_sequence = sequence;

        trial.attempt_count += 1;
        trial.next_attempt_number = TrialAttemptNumber::from_value(attempt.number.value() + 1);
        trial.authoritative_attempt = attempt.id;
        trial.state = TrialState::ASSIGNED;
        trial.updated_sequence = sequence;

        TrialAssignment assignment;
        assignment.envelope.epoch = impl.epoch;
        assignment.envelope.experiment = record.definition.id;
        assignment.envelope.generation = record.definition.generation;
        assignment.envelope.hypothesis = record.definition.hypothesis;
        assignment.envelope.hypothesis_revision = record.definition.hypothesis_revision;
        assignment.envelope.policy = record.definition.policy.id;
        assignment.envelope.policy_generation = record.definition.policy.generation;
        assignment.envelope.branch = trial.branch;
        assignment.envelope.branch_generation = trial.branch_generation;
        assignment.envelope.trial = trial.id;
        assignment.envelope.attempt = attempt.id;
        assignment.envelope.worker = request.worker;
        assignment.envelope.boot = request.boot;
        assignment.envelope.producer = live->producer;
        assignment.attempt_number = attempt.number;
        assignment.trial_payload = trial.payload;
        assignment.parameters = branch->parameters;
        // Observation and artifact identities are reserved here, in the
        // coordinator's identity namespace. A producer process never mints an
        // identity that the coordinator has not authorised.
        for (const MetricDefinition& metric : record.definition.metrics) {
          AssignedMetric reserved;
          reserved.metric = metric.id;
          reserved.observation_id = impl.alloc_observation_id();
          reserved.name = metric.name;
          reserved.kind = metric.kind;
          reserved.direction = metric.direction;
          reserved.required = metric.required;
          assignment.metrics.push_back(std::move(reserved));
        }
        const std::uint32_t artifact_budget = std::min<std::uint32_t>(
            impl.limits.max_artifacts_per_attempt,
            static_cast<std::uint32_t>(std::max<std::size_t>(
                0, impl.limits.max_artifacts_per_experiment - record.artifacts.size())));
        for (std::uint32_t index = 0; index < artifact_budget; ++index) {
          assignment.artifact_ids.push_back(impl.alloc_artifact_id());
        }
        assignment.planned_observations = static_cast<std::uint32_t>(assignment.metrics.size());

        record.attempts.emplace(attempt.id, attempt);
        reply.assignments.push_back(std::move(assignment));
        live->active_assignments += 1;
        live->total_assignments += 1;
        --budget;
      }
    }
    if (!reply.assignments.empty()) {
      const Status persisted = impl.persist_locked();
      if (!persisted.ok()) {
        reply.status = persisted;
        return reply;
      }
    }
    reply.status = Status::success();
  }
  return reply;
}

// ---------------------------------------------------------------------------
// Evidence publication
// ---------------------------------------------------------------------------

Status Coordinator::publish_observation(const PublishObservationRequest& request) {
  Impl& impl = *impl_;
  std::unique_lock<std::shared_mutex> lock(impl.mutex);

  const ExperimentRecord* probe = impl.find_experiment(request.observation.experiment);
  if (probe == nullptr) {
    return authority_error(ErrorCode::NOT_FOUND, "experiment does not exist");
  }
  const AuthorityEnvelope envelope =
      envelope_from(request.observation, request.epoch, request.worker, request.boot, *probe);
  EnvelopeCheck check = impl.check_envelope(envelope);
  if (!check.verdict.accepted) {
    ExperimentRecord* record = impl.find_experiment(request.observation.experiment);
    if (record != nullptr) {
      if (record->rejected_evidence.size() < 65536) {
        RejectedEvidence evidence;
        evidence.kind = "observation";
        evidence.identifier = request.observation.id.value();
        evidence.reason = check.verdict.code;
        evidence.detail = check.verdict.detail;
        record->rejected_evidence.push_back(std::move(evidence));
      }
      if (record->definition.late_evidence_policy == LateEvidencePolicy::RETAIN_HISTORICAL &&
          record->observations.size() < impl.limits.max_observations_per_experiment) {
        Observation historical = request.observation;
        historical.validity = ObservationValidity::INVALID;
        historical.detail = "rejected as non-authoritative: " + check.verdict.detail;
        historical.sequence = impl.next_sequence();
        const ObservationId id = historical.id;
        record->observations.push_back(std::move(historical));
        impl.observation_index[record->definition.id][id] = record->observations.size() - 1;
      }
      const Status persisted = impl.persist_locked();
      if (!persisted.ok()) {
        return persisted;
      }
    }
    return authority_error(check.verdict.code, check.verdict.detail);
  }

  ExperimentRecord& record = *check.record;
  TrialRecord& trial = *check.trial;
  const MetricDefinition* metric = impl.find_metric(record, request.observation.metric);
  if (metric == nullptr) {
    return observation_error("observation references a metric that is not defined by this experiment");
  }
  const std::map<ObservationId, std::size_t>& index = impl.observation_index[record.definition.id];
  const auto duplicate = index.find(request.observation.id);
  if (duplicate != index.end()) {
    const Observation& previous = record.observations[duplicate->second];
    if (same_observation(previous, request.observation)) {
      // Duplicate transport delivery of byte-identical evidence is harmless.
      return Status::success();
    }
    return make_error(ErrorCode::ALREADY_EXISTS, ErrorStage::IDENTITY,
                      "observation identity already exists with different content");
  }
  if (record.observations.size() >= impl.limits.max_observations_per_experiment) {
    return limits_error("experiment holds too many observations");
  }
  if (record.observations.size() >=
      static_cast<std::size_t>(impl.limits.max_observations_per_trial) *
          std::max<std::size_t>(1, record.trials.size())) {
    return limits_error("observation budget for this experiment is exhausted");
  }

  Observation observation = request.observation;
  if (observation.validity == ObservationValidity::VALID) {
    if (observation.value.kind != metric->kind) {
      return observation_error("observation value kind does not match the metric definition");
    }
    if (!is_finite_value(observation.value)) {
      if (!metric->accepts_non_finite) {
        return observation_error("non-finite observation rejected: the metric does not accept NaN or infinity");
      }
    } else if (observation.value.kind == MetricKind::REAL) {
      if (metric->lower_bound.has_value() && observation.value.real < *metric->lower_bound) {
        return observation_error("observation is below the configured validity bound");
      }
      if (metric->upper_bound.has_value() && observation.value.real > *metric->upper_bound) {
        return observation_error("observation is above the configured validity bound");
      }
    } else if (observation.value.kind == MetricKind::INTEGER) {
      if (metric->lower_bound.has_value() &&
          static_cast<double>(observation.value.integer) < *metric->lower_bound) {
        return observation_error("observation is below the configured validity bound");
      }
      if (metric->upper_bound.has_value() &&
          static_cast<double>(observation.value.integer) > *metric->upper_bound) {
        return observation_error("observation is above the configured validity bound");
      }
    } else if (observation.value.kind == MetricKind::CATEGORICAL && !metric->categories.empty()) {
      if (std::find(metric->categories.begin(), metric->categories.end(), observation.value.category) ==
          metric->categories.end()) {
        return observation_error("categorical observation is not one of the declared categories");
      }
    }
    // Uniqueness is scoped to the producing attempt. Evidence published by an
    // abandoned or superseded attempt is history, not current authority, and
    // must never block a fresh attempt from publishing its own observation.
    for (const Observation& other : record.observations) {
      if (other.trial == observation.trial && other.metric == observation.metric &&
          other.attempt == observation.attempt && other.validity == ObservationValidity::VALID) {
        return make_error(ErrorCode::ALREADY_EXISTS, ErrorStage::OBSERVATION,
                          "a valid observation for this metric already exists on this attempt");
      }
    }
  }

  observation.sequence = impl.next_sequence();
  const ObservationId id = observation.id;
  record.observations.push_back(std::move(observation));
  impl.observation_index[record.definition.id][id] = record.observations.size() - 1;
  if (trial.state == TrialState::ASSIGNED || trial.state == TrialState::RUNNING) {
    if (is_valid_transition(trial.state, TrialState::OBSERVING)) {
      trial.state = TrialState::OBSERVING;
      trial.updated_sequence = impl.next_sequence();
    }
  }
  if (check.attempt->state == AttemptState::ISSUED) {
    check.attempt->state = AttemptState::RUNNING;
    check.attempt->updated_sequence = impl.next_sequence();
  }
  const Status persisted = impl.persist_locked();
  if (!persisted.ok()) {
    record.observations.pop_back();
    impl.observation_index[record.definition.id].erase(id);
    return persisted;
  }
  return Status::success();
}

Status Coordinator::publish_artifact(const PublishArtifactRequest& request) {
  Impl& impl = *impl_;
  std::unique_lock<std::shared_mutex> lock(impl.mutex);

  const ExperimentRecord* probe = impl.find_experiment(request.artifact.experiment);
  if (probe == nullptr) {
    return authority_error(ErrorCode::NOT_FOUND, "experiment does not exist");
  }
  Observation shim;
  shim.experiment = request.artifact.experiment;
  shim.generation = request.artifact.generation;
  shim.branch = request.artifact.branch;
  shim.branch_generation = BranchGeneration{};
  shim.trial = request.artifact.trial;
  shim.attempt = request.artifact.attempt;
  shim.producer = ProducerId{};
  AuthorityEnvelope envelope = envelope_from(shim, request.epoch, request.worker, request.boot, *probe);
  {
    const auto branch_iterator = std::find_if(
        probe->definition.branches.begin(), probe->definition.branches.end(),
        [&request](const BranchDefinition& branch) { return branch.id == request.artifact.branch; });
    if (branch_iterator == probe->definition.branches.end()) {
      return authority_error(ErrorCode::STALE_BRANCH, "artifact references an unknown branch");
    }
    envelope.branch_generation = branch_iterator->generation;
  }
  {
    const auto attempt_iterator = probe->attempts.find(request.artifact.attempt);
    if (attempt_iterator == probe->attempts.end()) {
      return authority_error(ErrorCode::STALE_ATTEMPT, "artifact references an unknown attempt");
    }
    envelope.producer = attempt_iterator->second.producer;
  }
  EnvelopeCheck check = impl.check_envelope(envelope);
  if (!check.verdict.accepted) {
    ExperimentRecord* record = impl.find_experiment(request.artifact.experiment);
    if (record != nullptr) {
      if (record->rejected_evidence.size() < 65536) {
        RejectedEvidence evidence;
        evidence.kind = "artifact";
        evidence.identifier = request.artifact.id.value();
        evidence.reason = check.verdict.code;
        evidence.detail = check.verdict.detail;
        record->rejected_evidence.push_back(std::move(evidence));
      }
      const Status persisted = impl.persist_locked();
      if (!persisted.ok()) {
        return persisted;
      }
    }
    return authority_error(check.verdict.code, check.verdict.detail);
  }

  ExperimentRecord& record = *check.record;
  TrialRecord& trial = *check.trial;
  if (record.artifacts.size() >= impl.limits.max_artifacts_per_experiment) {
    return limits_error("experiment holds too many artifact references");
  }
  if (trial.artifacts.size() >= impl.limits.max_artifacts_per_attempt) {
    return limits_error("attempt holds too many artifact references");
  }
  if (request.artifact.locator.empty() ||
      request.artifact.locator.size() > impl.limits.max_locator_length) {
    return make_error(ErrorCode::INVALID_ARGUMENT, ErrorStage::OBSERVATION,
                      "artifact locator is empty or too long");
  }
  if (request.artifact.provenance.size() > impl.limits.max_provenance_length) {
    return make_error(ErrorCode::INVALID_ARGUMENT, ErrorStage::OBSERVATION,
                      "artifact provenance text is too long");
  }
  const std::map<ArtifactId, std::size_t>& index = impl.artifact_index[record.definition.id];
  const auto duplicate = index.find(request.artifact.id);
  if (duplicate != index.end()) {
    if (same_artifact(record.artifacts[duplicate->second], request.artifact)) {
      return Status::success();
    }
    return make_error(ErrorCode::ALREADY_EXISTS, ErrorStage::IDENTITY,
                      "artifact identity already exists with different content");
  }

  ArtifactRecord artifact = request.artifact;
  artifact.sequence = impl.next_sequence();
  artifact.current = true;
  const ArtifactId id = artifact.id;
  record.artifacts.push_back(std::move(artifact));
  impl.artifact_index[record.definition.id][id] = record.artifacts.size() - 1;
  trial.artifacts.push_back(id);
  trial.updated_sequence = impl.next_sequence();
  const Status persisted = impl.persist_locked();
  if (!persisted.ok()) {
    record.artifacts.pop_back();
    impl.artifact_index[record.definition.id].erase(id);
    trial.artifacts.pop_back();
    return persisted;
  }
  return Status::success();
}

Status Coordinator::commit_trial(const CommitTrialRequest& request) {
  Impl& impl = *impl_;
  std::unique_lock<std::shared_mutex> lock(impl.mutex);

  if (request.epoch != request.envelope.epoch || request.worker != request.envelope.worker ||
      request.boot != request.envelope.boot) {
    return protocol_error("commit envelope does not match the presenting transport identity");
  }
  EnvelopeCheck check = impl.check_envelope(request.envelope);
  if (!check.verdict.accepted) {
    return authority_error(check.verdict.code, check.verdict.detail);
  }
  ExperimentRecord& record = *check.record;
  TrialRecord& trial = *check.trial;
  AttemptRecord& attempt = *check.attempt;

  if (trial.state == TrialState::COMPLETED && attempt.committed) {
    // Duplicate delivery of the same authoritative completion is harmless and
    // must not create a second logical commit.
    return Status::success();
  }

  std::uint32_t observations = 0;
  for (const Observation& observation : record.observations) {
    if (observation.trial == trial.id && observation.attempt == attempt.id) {
      ++observations;
    }
  }
  if (observations == 0) {
    return make_error(ErrorCode::INSUFFICIENT_EVIDENCE, ErrorStage::OBSERVATION,
                      "trial completion requires at least one observation");
  }

  const std::uint64_t sequence = impl.next_sequence();
  std::vector<ArtifactId> registered;
  for (const ArtifactRecord& submitted : request.artifacts) {
    if (record.artifacts.size() >= impl.limits.max_artifacts_per_experiment) {
      return limits_error("experiment holds too many artifact references");
    }
    if (trial.artifacts.size() >= impl.limits.max_artifacts_per_attempt) {
      return limits_error("attempt holds too many artifact references");
    }
    if (submitted.locator.empty() || submitted.locator.size() > impl.limits.max_locator_length) {
      return make_error(ErrorCode::INVALID_ARGUMENT, ErrorStage::OBSERVATION,
                        "artifact locator is empty or too long");
    }
    ArtifactRecord artifact = submitted;
    artifact.experiment = record.definition.id;
    artifact.generation = record.definition.generation;
    artifact.branch = trial.branch;
    artifact.trial = trial.id;
    artifact.attempt = attempt.id;
    artifact.sequence = impl.next_sequence();
    artifact.current = true;
    const ArtifactId id = artifact.id;
    if (impl.artifact_index[record.definition.id].find(id) != impl.artifact_index[record.definition.id].end()) {
      return make_error(ErrorCode::ALREADY_EXISTS, ErrorStage::IDENTITY,
                        "artifact identity already exists in this experiment");
    }
    record.artifacts.push_back(std::move(artifact));
    impl.artifact_index[record.definition.id][id] = record.artifacts.size() - 1;
    trial.artifacts.push_back(id);
    registered.push_back(id);
  }

  if (!is_valid_transition(attempt.state, AttemptState::COMPLETED)) {
    for (const ArtifactId& id : registered) {
      impl.artifact_index[record.definition.id].erase(id);
    }
    while (!registered.empty()) {
      record.artifacts.pop_back();
      trial.artifacts.pop_back();
      registered.pop_back();
    }
    return invalid_transition_status("attempt", to_string(attempt.state), "COMPLETED");
  }
  attempt.state = AttemptState::COMPLETED;
  attempt.committed = true;
  attempt.updated_sequence = sequence;

  trial.state = TrialState::COMPLETED;
  trial.committed_attempts = 1;
  trial.failure_kind = FailureKind::NONE;
  trial.failure_detail.clear();
  trial.updated_sequence = sequence;
  impl.log(record, "trial-committed trial=" + trial.id.to_string() + " attempt=" + attempt.id.to_string());
  (void)request.result_payload;

  const Status persisted = impl.persist_locked();
  if (!persisted.ok()) {
    return persisted;
  }
  return Status::success();
}

Status Coordinator::fail_trial(const FailTrialRequest& request) {
  Impl& impl = *impl_;
  std::unique_lock<std::shared_mutex> lock(impl.mutex);

  if (request.epoch != request.envelope.epoch || request.worker != request.envelope.worker ||
      request.boot != request.envelope.boot) {
    return protocol_error("failure envelope does not match the presenting transport identity");
  }
  EnvelopeCheck check = impl.check_envelope(request.envelope);
  if (!check.verdict.accepted) {
    return authority_error(check.verdict.code, check.verdict.detail);
  }
  ExperimentRecord& record = *check.record;
  TrialRecord& trial = *check.trial;
  AttemptRecord& attempt = *check.attempt;
  if (request.kind == FailureKind::NONE) {
    return make_error(ErrorCode::INVALID_ARGUMENT, ErrorStage::LIFECYCLE,
                      "a failure must name a non-NONE failure kind");
  }
  const std::uint64_t sequence = impl.next_sequence();
  attempt.state = AttemptState::FAILED;
  attempt.failure_kind = request.kind;
  attempt.failure_detail = request.detail;
  attempt.updated_sequence = sequence;

  const bool retry_allowed = request.retryable && record.definition.retry_policy != RetryPolicy::NO_RETRY &&
                             trial.attempt_count < record.definition.max_attempts_per_trial;
  if (retry_allowed) {
    trial.state = TrialState::READY;
    trial.authoritative_attempt = TrialAttemptId{};
    trial.next_attempt_number = TrialAttemptNumber::from_value(trial.attempt_count + 1);
    trial.failure_kind = request.kind;
    trial.failure_detail = request.detail;
  } else {
    trial.state = TrialState::FAILED;
    trial.failure_kind = request.kind;
    trial.failure_detail = request.detail;
  }
  trial.updated_sequence = sequence;

  if (record.definition.partial_failure_policy == PartialFailurePolicy::INVALIDATE_BRANCH) {
    const auto branch_iterator =
        std::find_if(record.definition.branches.begin(), record.definition.branches.end(),
                     [&trial](const BranchDefinition& branch) { return branch.id == trial.branch; });
    if (branch_iterator != record.definition.branches.end() &&
        is_valid_transition(branch_iterator->state, BranchState::INVALID)) {
      branch_iterator->state = BranchState::INVALID;
      impl.log(record, "branch-invalidated branch=" + trial.branch.to_string() + " reason=" + request.detail);
    }
  }
  impl.log(record, "trial-failed trial=" + trial.id.to_string() + " kind=" + std::string(to_string(request.kind)));

  const Status persisted = impl.persist_locked();
  if (!persisted.ok()) {
    return persisted;
  }
  return Status::success();
}

// ---------------------------------------------------------------------------
// Cancellation
// ---------------------------------------------------------------------------

Status Coordinator::cancel_trial(TrialId trial_id, std::string reason) {
  Impl& impl = *impl_;
  {
    std::unique_lock<std::shared_mutex> lock(impl.mutex);
    ExperimentRecord* owner = nullptr;
    TrialRecord* trial = nullptr;
    for (auto& entry : impl.state.experiments) {
      const auto iterator = entry.second.trials.find(trial_id);
      if (iterator != entry.second.trials.end()) {
        owner = &entry.second;
        trial = &iterator->second;
        break;
      }
    }
    if (owner == nullptr || trial == nullptr) {
      return make_error(ErrorCode::NOT_FOUND, ErrorStage::LIFECYCLE, "trial does not exist");
    }
    if (is_terminal(trial->state)) {
      if (trial->state == TrialState::CANCELLED) {
        return Status::success();
      }
      return invalid_transition_status("trial", to_string(trial->state), "CANCELLED");
    }
    const std::uint64_t sequence = impl.next_sequence();
    trial->state = TrialState::CANCELLED;
    trial->failure_kind = FailureKind::CANCELLATION;
    trial->failure_detail = reason;
    trial->updated_sequence = sequence;
    for (auto& attempt_entry : owner->attempts) {
      AttemptRecord& attempt = attempt_entry.second;
      if (attempt.trial != trial_id || is_attempt_terminal(attempt.state)) {
        continue;
      }
      attempt.state = AttemptState::CANCELLED;
      attempt.failure_kind = FailureKind::CANCELLATION;
      attempt.failure_detail = reason;
      attempt.updated_sequence = sequence;
    }
    owner->revision_sequence = sequence;
    impl.log(*owner, "trial-cancelled trial=" + trial_id.to_string() + " reason=" + reason);
    const Status persisted = impl.persist_locked();
    if (!persisted.ok()) {
      return persisted;
    }
  }
  impl.notify_event("trial-cancelled:" + trial_id.to_string());
  return Status::success();
}

Status Coordinator::cancel_experiment(ExperimentId experiment, std::string reason) {
  Impl& impl = *impl_;
  {
    std::unique_lock<std::shared_mutex> lock(impl.mutex);
    ExperimentRecord* record = impl.find_experiment(experiment);
    if (record == nullptr) {
      return make_error(ErrorCode::NOT_FOUND, ErrorStage::LIFECYCLE, "experiment does not exist");
    }
    if (record->state == ExperimentState::CANCELLED) {
      return Status::success();
    }
    if (!is_valid_transition(record->state, ExperimentState::CANCELLED)) {
      return invalid_transition_status("experiment", to_string(record->state), "CANCELLED");
    }
    const std::uint64_t sequence = impl.next_sequence();
    record->state = ExperimentState::CANCELLED;
    for (auto& trial_entry : record->trials) {
      TrialRecord& trial = trial_entry.second;
      if (is_terminal(trial.state)) {
        continue;
      }
      trial.state = TrialState::CANCELLED;
      trial.failure_kind = FailureKind::CANCELLATION;
      trial.failure_detail = reason;
      trial.updated_sequence = sequence;
    }
    for (auto& attempt_entry : record->attempts) {
      AttemptRecord& attempt = attempt_entry.second;
      if (is_attempt_terminal(attempt.state)) {
        continue;
      }
      attempt.state = AttemptState::CANCELLED;
      attempt.failure_kind = FailureKind::CANCELLATION;
      attempt.failure_detail = reason;
      attempt.updated_sequence = sequence;
    }
    record->revision_sequence = sequence;
    impl.log(*record, "experiment-cancelled reason=" + reason);
    const Status persisted = impl.persist_locked();
    if (!persisted.ok()) {
      return persisted;
    }
  }
  impl.notify_event("experiment-cancelled:" + experiment.to_string());
  return Status::success();
}

}  // namespace experiment_fabric
