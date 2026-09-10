// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "scenarios.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

#include "experiment_fabric/canonical.hpp"
#include "experiment_fabric/hash.hpp"

namespace experiment_fabric::scenarios {
namespace {

MetricSpec real_metric(std::string name, std::string unit, MetricDirection direction, bool required = true) {
  MetricSpec metric;
  metric.name = std::move(name);
  metric.unit = std::move(unit);
  metric.kind = MetricKind::REAL;
  metric.direction = direction;
  metric.aggregation = AggregationRule::MEAN;
  metric.required = required;
  return metric;
}

BranchSpec branch(std::string name, BranchRole role, std::uint32_t planned_trials,
                  std::vector<ParameterAssignment> parameters) {
  BranchSpec spec;
  spec.name = std::move(name);
  spec.role = role;
  spec.planned_trials = planned_trials;
  spec.parameters = std::move(parameters);
  spec.environment = std::string("reference-environment");
  spec.environment_fingerprint = "synthetic-reference-x64";
  spec.input_set = std::string("reference-input-set-v1");
  return spec;
}

ComparisonRuleSpec rule(std::string metric, ComparisonOp op, double threshold, std::uint32_t priority) {
  ComparisonRuleSpec spec;
  spec.metric = std::move(metric);
  spec.op = op;
  spec.threshold = threshold;
  spec.priority = priority;
  return spec;
}

/// Deterministic jitter in [-spread, +spread] derived from the trial seed.
double jitter(std::optional<std::uint64_t> seed, double spread) {
  const std::uint64_t value = seed.value_or(0);
  DeterministicRng rng(value);
  const double unit = static_cast<double>(rng.next_u64() % 1000000ull) / 1000000.0;
  return (unit * 2.0 - 1.0) * spread;
}

double parameter_as_real(const WorkerTaskContext& context, const std::string& key, double fallback) {
  const std::string* value = context.parameter(key);
  if (value == nullptr) {
    return fallback;
  }
  try {
    return std::stod(*value);
  } catch (...) {
    return fallback;
  }
}

bool parameter_as_bool(const WorkerTaskContext& context, const std::string& key) {
  const std::string* value = context.parameter(key);
  return value != nullptr && (*value == "true" || *value == "1");
}

/// Bounded deterministic CPU work, used to hold a worker inside a trial so that
/// an external process kill lands mid-execution rather than between requests.
std::uint64_t busy_work(std::uint64_t seed, std::uint32_t rounds) {
  Sha256 hasher;
  hasher.update_u64(seed);
  Sha256::Digest digest = hasher.finalize();
  for (std::uint32_t round = 0; round < rounds; ++round) {
    Sha256 next;
    next.update(digest);
    digest = next.finalize();
  }
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < 8; ++index) {
    value = (value << 8) | digest[index];
  }
  return value;
}

}  // namespace

std::vector<std::string> scenario_names() {
  return {"latency", "throughput-memory", "forked-parameter", "insufficient", "invalid-evidence", "authority",
          "retry"};
}

std::vector<std::string> workload_names() { return {"parameter", "parameter-slow", "fail-once", "partial"}; }

bool build_scenario(const std::string& name, ExperimentSpec& out, std::string& error) {
  ExperimentSpec spec;
  if (name == "latency") {
    spec.name = "baseline-vs-candidate-latency";
    spec.hypothesis_statement =
        "Raising the prefetch depth lowers mean request latency without changing the input set.";
    spec.expected_direction = MetricDirection::MINIMIZE;
    spec.expected_metric = std::string("latency_ms");
    spec.provenance = "synthetic reference experiment";
    spec.metrics = {real_metric("latency_ms", "ms", MetricDirection::MINIMIZE)};
    spec.branches = {
        branch("baseline", BranchRole::BASELINE, 2, {make_real_parameter("latency_ms", 120.0)}),
        branch("candidate", BranchRole::CANDIDATE, 2, {make_real_parameter("latency_ms", 95.0)}),
    };
    spec.rules = {rule("latency_ms", ComparisonOp::MIN_IMPROVEMENT, 1.0, 0)};
    spec.min_completed_trials_per_branch = 2;
    spec.min_valid_observations_per_required_metric = 1;
    spec.experiment_seed = 4242;
    spec.max_attempts_per_trial = 3;
  } else if (name == "throughput-memory") {
    spec.name = "throughput-versus-memory-tradeoff";
    spec.hypothesis_statement =
        "Batching raises throughput while peak resident memory must not regress beyond the tolerance.";
    spec.provenance = "synthetic reference experiment";
    spec.metrics = {real_metric("throughput", "ops/s", MetricDirection::MAXIMIZE),
                    real_metric("peak_memory_mb", "MiB", MetricDirection::MINIMIZE)};
    std::vector<ParameterAssignment> baseline_parameters = {make_real_parameter("throughput", 1000.0),
                                                            make_real_parameter("peak_memory_mb", 512.0)};
    std::vector<ParameterAssignment> candidate_parameters = {make_real_parameter("throughput", 1250.0),
                                                             make_real_parameter("peak_memory_mb", 530.0)};
    spec.branches = {branch("baseline", BranchRole::BASELINE, 2, std::move(baseline_parameters)),
                     branch("candidate", BranchRole::CANDIDATE, 2, std::move(candidate_parameters))};
    spec.rules = {rule("throughput", ComparisonOp::MIN_IMPROVEMENT, 50.0, 0),
                  rule("peak_memory_mb", ComparisonOp::MAX_REGRESSION_TOLERANCE, 32.0, 1)};
    spec.min_completed_trials_per_branch = 2;
    spec.experiment_seed = 77;
  } else if (name == "forked-parameter") {
    spec.name = "forked-branch-with-changed-parameter";
    spec.hypothesis_statement = "A forked branch with a larger batch size improves throughput.";
    spec.provenance = "synthetic reference experiment";
    spec.metrics = {real_metric("throughput", "ops/s", MetricDirection::MAXIMIZE)};
    BranchSpec candidate = branch("candidate", BranchRole::CANDIDATE, 2, {make_real_parameter("throughput", 1180.0)});
    candidate.fork_from = std::string("baseline");
    spec.branches = {branch("baseline", BranchRole::BASELINE, 2, {make_real_parameter("throughput", 1000.0)}),
                     candidate};
    spec.rules = {rule("throughput", ComparisonOp::MIN_IMPROVEMENT, 10.0, 0)};
    spec.min_completed_trials_per_branch = 2;
    spec.experiment_seed = 9;
  } else if (name == "insufficient") {
    spec.name = "favorable-candidate-with-insufficient-evidence";
    spec.hypothesis_statement = "A much lower latency is claimed, but the evidence floor is not met.";
    spec.provenance = "synthetic reference experiment";
    spec.metrics = {real_metric("latency_ms", "ms", MetricDirection::MINIMIZE)};
    spec.branches = {branch("baseline", BranchRole::BASELINE, 1, {make_real_parameter("latency_ms", 120.0)}),
                     branch("candidate", BranchRole::CANDIDATE, 1, {make_real_parameter("latency_ms", 40.0)})};
    spec.rules = {rule("latency_ms", ComparisonOp::MIN_IMPROVEMENT, 1.0, 0)};
    // The policy demands four authoritative completed trials per branch while
    // the branches plan only one each.
    spec.min_completed_trials_per_branch = 4;
    spec.experiment_seed = 5;
  } else if (name == "invalid-evidence") {
    spec.name = "numerically-favorable-but-invalid-evidence";
    spec.hypothesis_statement = "A favorable number is produced without a valid observation.";
    spec.provenance = "synthetic reference experiment";
    spec.metrics = {real_metric("latency_ms", "ms", MetricDirection::MINIMIZE)};
    std::vector<ParameterAssignment> candidate_parameters = {make_real_parameter("latency_ms", 30.0),
                                                             make_boolean_parameter("latency_ms_invalid", true)};
    spec.branches = {branch("baseline", BranchRole::BASELINE, 2, {make_real_parameter("latency_ms", 120.0)}),
                     branch("candidate", BranchRole::CANDIDATE, 2, std::move(candidate_parameters))};
    spec.rules = {rule("latency_ms", ComparisonOp::MIN_IMPROVEMENT, 1.0, 0)};
    spec.min_completed_trials_per_branch = 2;
    spec.reject_on_any_invalid_observation = true;
    spec.experiment_seed = 11;
  } else if (name == "authority") {
    spec.name = "authority-fencing-under-process-death";
    spec.hypothesis_statement = "The candidate reduces latency once its trials are authoritative.";
    spec.provenance = "synthetic reference experiment";
    spec.metrics = {real_metric("latency_ms", "ms", MetricDirection::MINIMIZE)};
    spec.branches = {branch("baseline", BranchRole::BASELINE, 1, {make_real_parameter("latency_ms", 120.0)}),
                     branch("candidate", BranchRole::CANDIDATE, 1, {make_real_parameter("latency_ms", 95.0)})};
    spec.rules = {rule("latency_ms", ComparisonOp::MIN_IMPROVEMENT, 1.0, 0)};
    spec.min_completed_trials_per_branch = 1;
    spec.max_attempts_per_trial = 4;
    spec.experiment_seed = 2026;
  } else if (name == "retry") {
    spec.name = "failed-trial-followed-by-fresh-attempt";
    spec.hypothesis_statement = "A transient execution failure is recoverable through a fresh attempt.";
    spec.provenance = "synthetic reference experiment";
    spec.metrics = {real_metric("latency_ms", "ms", MetricDirection::MINIMIZE)};
    spec.branches = {branch("baseline", BranchRole::BASELINE, 1, {make_real_parameter("latency_ms", 120.0)}),
                     branch("candidate", BranchRole::CANDIDATE, 1, {make_real_parameter("latency_ms", 95.0)})};
    spec.rules = {rule("latency_ms", ComparisonOp::MIN_IMPROVEMENT, 1.0, 0)};
    spec.min_completed_trials_per_branch = 1;
    spec.max_attempts_per_trial = 3;
    spec.experiment_seed = 31;
  } else {
    error = "unknown scenario: " + name;
    return false;
  }
  out = std::move(spec);
  return true;
}

Status create_planned_trials(Coordinator& coordinator, ExperimentId experiment, std::string payload) {
  const auto snapshot = coordinator.snapshot(experiment);
  if (!snapshot.ok()) {
    return snapshot.status();
  }
  for (const BranchDefinition& branch : snapshot.value().definition.branches) {
    if (!is_branch_eligible(branch.state)) {
      continue;
    }
    for (std::uint32_t index = 0; index < branch.planned_trials; ++index) {
      const auto created = coordinator.create_trial(experiment, branch.id, payload);
      if (!created.ok()) {
        return created.status();
      }
    }
  }
  return Status::success();
}

TrialOutcome run_workload(const std::string& name, const TrialAssignment& assignment,
                          const WorkerTaskContext& context) {
  TrialOutcome outcome;
  if (name == "fail-once" && context.attempt.value() <= 1) {
    outcome.succeeded = false;
    outcome.failure_kind = FailureKind::EXECUTION_FAILURE;
    outcome.failure_detail = "synthetic transient execution failure on attempt 1";
    outcome.retryable = true;
    return outcome;
  }
  if (name == "parameter-slow") {
    const std::uint64_t seed = assignment.envelope.trial.value() ^ context.boot.value();
    const std::uint64_t mixed = busy_work(seed, 6000000);
    if (mixed == 0) {
      outcome.succeeded = false;
      outcome.failure_kind = FailureKind::EXECUTION_FAILURE;
      outcome.failure_detail = "unreachable synthetic guard";
      return outcome;
    }
  }

  const std::optional<std::uint64_t> trial_seed =
      assignment.envelope.trial.valid() ? std::optional<std::uint64_t>(assignment.envelope.trial.value() * 2654435761ull)
                                        : std::nullopt;
  outcome.succeeded = true;
  std::size_t emitted = 0;
  for (const AssignedMetric& metric : assignment.metrics) {
    if (name == "partial" && !metric.required) {
      continue;
    }
    if (name == "partial" && metric.required && emitted >= 1) {
      continue;
    }
    NamedObservation observation;
    observation.metric = metric.name;
    observation.value.kind = metric.kind;
    const double base = parameter_as_real(context, metric.name, 100.0);
    const double jittered = base * (1.0 + jitter(trial_seed, 0.02));
    if (parameter_as_bool(context, metric.name + "_invalid")) {
      observation.validity = ObservationValidity::INVALID;
      observation.value.real = jittered;
      observation.detail = "synthetic workload produced an invalid observation by configuration";
    } else {
      observation.validity = ObservationValidity::VALID;
      observation.value.real = jittered;
    }
    outcome.observations.push_back(std::move(observation));
    ++emitted;
  }
  outcome.result_payload = "trial=" + assignment.envelope.trial.to_string() +
                           " attempt=" + assignment.envelope.attempt.to_string();
  return outcome;
}


// ---------------------------------------------------------------------------
// In-process driver helpers
// ---------------------------------------------------------------------------

Result<Session> join(Coordinator& coordinator, std::uint64_t identity, std::uint64_t boot) {
  RegisterWorkerRequest request;
  request.worker = WorkerId::from_value(identity);
  request.boot = WorkerBootId::from_value(boot == 0 ? (0x5E55100000000000ull | identity) : boot);
  request.endpoint = "example-session-" + std::to_string(identity);
  request.max_concurrent_assignments = 32;
  request.fingerprint = "reference-example-worker";
  auto reply = coordinator.register_worker(request);
  if (!reply.ok()) {
    return reply.status();
  }
  if (reply.value().status.failed()) {
    return reply.value().status;
  }
  Session session;
  session.worker = request.worker;
  session.boot = reply.value().boot;
  session.producer = reply.value().producer;
  session.epoch = reply.value().epoch;
  return session;
}

Result<TrialAssignment> claim(Coordinator& coordinator, const Session& session, std::uint32_t max_claims) {
  ClaimTrialRequest request;
  request.epoch = session.epoch;
  request.worker = session.worker;
  request.boot = session.boot;
  request.max_claims = max_claims;
  auto reply = coordinator.claim_trials(request);
  if (!reply.ok()) {
    return reply.status();
  }
  if (reply.value().status.failed()) {
    return reply.value().status;
  }
  if (reply.value().assignments.empty()) {
    return make_error(ErrorCode::NOT_FOUND, ErrorStage::LIFECYCLE, "no trial is available to claim");
  }
  return reply.value().assignments.front();
}

Status publish(Coordinator& coordinator, const Session& session, const TrialAssignment& assignment,
               std::string_view metric, double value, ObservationValidity validity) {
  const AssignedMetric* reservation = nullptr;
  for (const AssignedMetric& candidate : assignment.metrics) {
    if (candidate.name == metric) {
      reservation = &candidate;
    }
  }
  if (reservation == nullptr) {
    return make_error(ErrorCode::NOT_FOUND, ErrorStage::OBSERVATION,
                      "the requested metric was not reserved for this assignment");
  }
  PublishObservationRequest request;
  request.epoch = session.epoch;
  request.worker = session.worker;
  request.boot = session.boot;
  request.observation.id = reservation->observation_id;
  request.observation.experiment = assignment.envelope.experiment;
  request.observation.generation = assignment.envelope.generation;
  request.observation.branch = assignment.envelope.branch;
  request.observation.branch_generation = assignment.envelope.branch_generation;
  request.observation.trial = assignment.envelope.trial;
  request.observation.attempt = assignment.envelope.attempt;
  request.observation.metric = reservation->metric;
  request.observation.producer = session.producer;
  request.observation.worker = session.worker;
  request.observation.boot = session.boot;
  request.observation.epoch = session.epoch;
  request.observation.validity = validity;
  request.observation.value.kind = reservation->kind;
  request.observation.value.real = value;
  request.observation.value.integer = static_cast<std::int64_t>(value);
  request.observation.value.boolean = value != 0.0;
  return coordinator.publish_observation(request);
}

Status commit(Coordinator& coordinator, const Session& session, const TrialAssignment& assignment) {
  CommitTrialRequest request;
  request.epoch = session.epoch;
  request.worker = session.worker;
  request.boot = session.boot;
  request.envelope = assignment.envelope;
  request.result_payload = "reference-example";
  return coordinator.commit_trial(request);
}

Status complete_one(Coordinator& coordinator, const Session& session, std::string_view metric, double value,
                    ObservationValidity validity) {
  auto assignment = claim(coordinator, session);
  if (!assignment.ok()) {
    return assignment.status();
  }
  const Status published = publish(coordinator, session, assignment.value(), metric, value, validity);
  if (!published.ok()) {
    return published;
  }
  return commit(coordinator, session, assignment.value());
}

}  // namespace experiment_fabric::scenarios
