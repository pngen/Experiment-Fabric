// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <algorithm>
#include <utility>

#include "coordinator_internal.hpp"
#include "experiment_fabric/inspect.hpp"
#include "experiment_fabric/version.hpp"

namespace experiment_fabric {
namespace {

Status bad_argument(std::string reason) {
  return make_error(ErrorCode::INVALID_ARGUMENT, ErrorStage::VALIDATION, std::move(reason));
}

}  // namespace

Result<std::vector<ExperimentId>> Coordinator::list_experiments() const {
  Impl& impl = *impl_;
  std::shared_lock<std::shared_mutex> lock(impl.mutex);
  std::vector<ExperimentId> experiments;
  experiments.reserve(impl.state.experiments.size());
  for (const auto& entry : impl.state.experiments) {
    experiments.push_back(entry.first);
  }
  return experiments;
}

Result<ExperimentSnapshot> Coordinator::snapshot(ExperimentId experiment) const {
  Impl& impl = *impl_;
  std::shared_lock<std::shared_mutex> lock(impl.mutex);
  const ExperimentRecord* record = impl.find_experiment(experiment);
  if (record == nullptr) {
    return make_error(ErrorCode::NOT_FOUND, ErrorStage::VALIDATION, "experiment does not exist");
  }
  ExperimentSnapshot snapshot;
  snapshot.definition = record->definition;
  snapshot.hypothesis = record->hypothesis;
  snapshot.hypothesis_history = record->hypothesis_history;
  snapshot.state = record->state;
  snapshot.producing_epoch = impl.epoch;
  snapshot.revision_sequence = record->revision_sequence;
  snapshot.observations = record->observations;
  snapshot.artifacts = record->artifacts;
  snapshot.decisions = record->decisions;
  snapshot.rollback_points = record->rollback_points;
  snapshot.rejected_evidence = record->rejected_evidence;
  snapshot.lifecycle_log = record->lifecycle_log;
  snapshot.state_digest = impl.state.experiment_digest(experiment);
  for (const auto& entry : record->trials) {
    TrialSnapshot trial;
    trial.trial = entry.second;
    for (const auto& attempt_entry : record->attempts) {
      if (attempt_entry.second.trial == entry.first) {
        trial.attempts.push_back(attempt_entry.second);
      }
    }
    std::sort(trial.attempts.begin(), trial.attempts.end(),
              [](const AttemptRecord& lhs, const AttemptRecord& rhs) { return lhs.id < rhs.id; });
    snapshot.trials.push_back(std::move(trial));
  }
  std::sort(snapshot.trials.begin(), snapshot.trials.end(),
            [](const TrialSnapshot& lhs, const TrialSnapshot& rhs) { return lhs.trial.id < rhs.trial.id; });
  std::sort(snapshot.observations.begin(), snapshot.observations.end(),
            [](const Observation& lhs, const Observation& rhs) { return lhs.id < rhs.id; });
  std::sort(snapshot.artifacts.begin(), snapshot.artifacts.end(),
            [](const ArtifactRecord& lhs, const ArtifactRecord& rhs) { return lhs.id < rhs.id; });
  std::sort(snapshot.decisions.begin(), snapshot.decisions.end(),
            [](const Decision& lhs, const Decision& rhs) { return lhs.id < rhs.id; });
  std::sort(snapshot.rollback_points.begin(), snapshot.rollback_points.end(),
            [](const RollbackPoint& lhs, const RollbackPoint& rhs) { return lhs.id < rhs.id; });
  std::sort(snapshot.hypothesis_history.begin(), snapshot.hypothesis_history.end(),
            [](const Hypothesis& lhs, const Hypothesis& rhs) { return lhs.revision < rhs.revision; });
  return snapshot;
}

Result<ReproducibilityRecord> Coordinator::reproducibility(ExperimentId experiment, BranchId branch_id) const {
  Impl& impl = *impl_;
  std::shared_lock<std::shared_mutex> lock(impl.mutex);
  const ExperimentRecord* record = impl.find_experiment(experiment);
  if (record == nullptr) {
    return make_error(ErrorCode::NOT_FOUND, ErrorStage::VALIDATION, "experiment does not exist");
  }
  const BranchDefinition* branch = impl.find_branch(*record, branch_id);
  if (branch == nullptr) {
    return make_error(ErrorCode::NOT_FOUND, ErrorStage::VALIDATION, "branch does not exist");
  }
  ReproducibilityRecord result;
  result.experiment = record->definition.id;
  result.generation = record->definition.generation;
  result.hypothesis = record->definition.hypothesis;
  result.hypothesis_revision = record->definition.hypothesis_revision;
  result.branch = branch->id;
  result.branch_generation = branch->generation;
  result.policy = record->definition.policy.id;
  result.policy_generation = record->definition.policy.generation;
  result.definition_digest = canonical::definition_digest(record->definition);
  result.branch_definition_digest = canonical::branch_digest(*branch);
  result.policy_digest = canonical::policy_digest(record->definition.policy);
  result.input_set = branch->input_set;
  result.input_set_present = branch->input_set_present;
  result.environment = branch->environment;
  result.environment_present = branch->environment_present;
  result.environment_fingerprint = branch->environment_fingerprint;
  result.seed = branch->seed;
  result.parameters = branch->parameters;

  for (const auto& entry : record->trials) {
    const TrialRecord& trial = entry.second;
    if (trial.branch != branch_id || trial.state != TrialState::COMPLETED) {
      continue;
    }
    if (trial.generation != record->definition.generation || trial.committed_attempts != 1) {
      continue;
    }
    for (const ArtifactId& artifact : trial.artifacts) {
      result.artifacts.push_back(artifact);
    }
    const auto attempt_iterator = record->attempts.find(trial.authoritative_attempt);
    if (attempt_iterator != record->attempts.end()) {
      result.producer_identity = "producer:" + attempt_iterator->second.producer.to_string() +
                                 " worker:" + attempt_iterator->second.worker.to_string() +
                                 " boot:" + attempt_iterator->second.boot.to_string();
    }
    if (!trial.seed.has_value() && result.seed.has_value()) {
      result.seed = trial.seed;
    }
  }
  std::sort(result.artifacts.begin(), result.artifacts.end());
  for (const Observation& observation : record->observations) {
    if (observation.branch == branch_id && observation.validity == ObservationValidity::VALID) {
      ++result.observation_count;
    }
  }

  if (!record->definition.provenance.empty()) {
    result.executable_identity = record->definition.provenance;
  }
  if (!branch->environment_fingerprint.empty()) {
    result.environment_fingerprint = branch->environment_fingerprint;
  } else {
    result.missing_materials.push_back("environment-fingerprint");
  }
  if (!branch->input_set_present) {
    result.missing_materials.push_back("input-set-identity");
  }
  if (!branch->seed.has_value() && !record->definition.experiment_seed.has_value()) {
    result.missing_materials.push_back("seed");
  }
  if (result.producer_identity.empty()) {
    result.missing_materials.push_back("producer-identity");
  }
  if (result.artifacts.empty()) {
    result.missing_materials.push_back("artifact-references");
  }
  if (result.environment_fingerprint.empty()) {
    result.missing_materials.push_back("executable-identity");
  }
  std::sort(result.missing_materials.begin(), result.missing_materials.end());
  result.missing_materials.erase(std::unique(result.missing_materials.begin(), result.missing_materials.end()),
                                 result.missing_materials.end());

  if (result.missing_materials.empty()) {
    result.status = ReproducibilityStatus::REPRODUCIBLE;
  } else if (result.missing_materials.size() <= 2 && result.seed.has_value()) {
    result.status = ReproducibilityStatus::PARTIALLY_REPRODUCIBLE;
  } else if (!result.seed.has_value() && result.missing_materials.size() >= 3) {
    result.status = ReproducibilityStatus::NOT_REPRODUCIBLE;
  } else {
    result.status = ReproducibilityStatus::PARTIALLY_REPRODUCIBLE;
  }
  return result;
}

Result<std::vector<std::string>> Coordinator::worker_lines() const {
  Impl& impl = *impl_;
  std::shared_lock<std::shared_mutex> lock(impl.mutex);
  std::vector<std::string> lines;
  lines.push_back("epoch " + impl.epoch.to_string());
  std::vector<const LiveWorker*> workers;
  workers.reserve(impl.live_workers.size());
  for (const auto& entry : impl.live_workers) {
    workers.push_back(&entry.second);
  }
  std::sort(workers.begin(), workers.end(), [](const LiveWorker* lhs, const LiveWorker* rhs) {
    if (lhs->worker != rhs->worker) {
      return lhs->worker < rhs->worker;
    }
    return lhs->boot < rhs->boot;
  });
  for (const LiveWorker* worker : workers) {
    std::string line = "worker ";
    line.append(worker->worker.to_string());
    line.append(" boot ");
    line.append(worker->boot.to_string());
    line.append(" producer ");
    line.append(worker->producer.to_string());
    line.append(worker->revoked ? " REVOKED" : " LIVE");
    line.append(" active ");
    line.append(std::to_string(worker->active_assignments));
    line.append(" total ");
    line.append(std::to_string(worker->total_assignments));
    if (!worker->endpoint.empty()) {
      line.append(" endpoint ");
      line.append(worker->endpoint);
    }
    lines.push_back(std::move(line));
  }
  return lines;
}

Result<std::vector<std::string>> Coordinator::query_lines(QueryKind kind, const std::string& argument) const {
  Impl& impl = *impl_;
  if (kind == QueryKind::STATUS) {
    std::shared_lock<std::shared_mutex> lock(impl.mutex);
    std::vector<std::string> lines;
    lines.push_back(std::string("experiment-fabric ") + std::string(version_string()));
    lines.push_back("coordinator-epoch " + impl.epoch.to_string());
    lines.push_back("sequence " + std::to_string(impl.current_sequence()));
    lines.push_back(std::string("admitting-work ") + (impl.admitting_work ? "true" : "false"));
    lines.push_back(std::string("persistence-degraded ") + (impl.persistence_degraded ? "true" : "false"));
    lines.push_back("experiments " + std::to_string(impl.state.experiments.size()));
    lines.push_back("worker-incarnations " + std::to_string(impl.state.workers.size()));
    lines.push_back("live-workers " + std::to_string(impl.live_workers.size()));
    return lines;
  }
  if (kind == QueryKind::LIST_EXPERIMENTS) {
    std::shared_lock<std::shared_mutex> lock(impl.mutex);
    std::vector<std::string> lines;
    for (const auto& entry : impl.state.experiments) {
      const ExperimentRecord& record = entry.second;
      std::string line = "experiment ";
      line.append(entry.first.to_string());
      line.append(" generation ");
      line.append(record.definition.generation.to_string());
      line.append(" state ");
      line.append(to_string(record.state));
      line.append(" branches ");
      line.append(std::to_string(record.definition.branches.size()));
      line.append(" trials ");
      line.append(std::to_string(record.trials.size()));
      line.append(" name ");
      line.append(record.definition.name);
      lines.push_back(std::move(line));
    }
    return lines;
  }
  if (kind == QueryKind::WORKERS) {
    return worker_lines();
  }
  if (kind == QueryKind::VALIDATE_STATE) {
    const Status status = validate_state();
    std::vector<std::string> lines;
    lines.push_back(std::string("validate-state ") + (status.ok() ? "OK" : "FAILED"));
    if (status.failed()) {
      lines.push_back(status.to_string());
    }
    const auto digest = state_digest_hex();
    if (digest.ok()) {
      lines.push_back("state-digest " + digest.value());
    } else {
      lines.push_back(std::string("state-digest unavailable: ") + digest.status().to_string());
    }
    return lines;
  }

  // Remaining queries operate on one experiment, optionally qualified by a
  // branch using the canonical "experiment:branch" form.
  std::string experiment_text = argument;
  std::string branch_text;
  const std::size_t separator = argument.find(':');
  if (separator != std::string::npos) {
    experiment_text = argument.substr(0, separator);
    branch_text = argument.substr(separator + 1);
  }
  if (experiment_text.empty()) {
    return bad_argument("a query of this kind requires an experiment identity argument");
  }
  const auto experiment = ExperimentId::parse(experiment_text);
  if (!experiment.has_value()) {
    return bad_argument("experiment identity argument is not canonical decimal text");
  }
  BranchId branch;
  if (!branch_text.empty()) {
    const auto parsed = BranchId::parse(branch_text);
    if (!parsed.has_value()) {
      return bad_argument("branch identity argument is not canonical decimal text");
    }
    branch = *parsed;
  }

  const Limits limits = impl.limits;
  if (kind == QueryKind::EXPLAIN_DECISION) {
    if (!branch.valid()) {
      return bad_argument("explain-decision requires the canonical experiment:branch form");
    }
    const auto decision = evaluate(*experiment, branch);
    if (!decision.ok()) {
      return decision.status();
    }
    return inspect::render_decision(decision.value(), limits);
  }
  const auto snapshot_value = snapshot(*experiment);
  if (!snapshot_value.ok()) {
    return snapshot_value.status();
  }
  switch (kind) {
    case QueryKind::EXPERIMENT_SNAPSHOT:
      return inspect::render_snapshot(snapshot_value.value(), limits);
    case QueryKind::HYPOTHESIS:
      return inspect::render_hypothesis(snapshot_value.value(), limits);
    case QueryKind::BRANCH_LINEAGE:
      return inspect::render_lineage(snapshot_value.value(), limits);
    case QueryKind::TRIALS:
      return inspect::render_trials(snapshot_value.value(), limits);
    case QueryKind::OBSERVATIONS:
      return inspect::render_observations(snapshot_value.value(), limits);
    case QueryKind::REJECTED_EVIDENCE:
      return inspect::render_rejected_evidence(snapshot_value.value(), limits);
    case QueryKind::ARTIFACTS:
      return inspect::render_artifacts(snapshot_value.value(), limits);
    case QueryKind::REPRODUCIBILITY: {
      if (!branch.valid()) {
        return bad_argument("reproducibility requires the canonical experiment:branch form");
      }
      const auto record = reproducibility(*experiment, branch);
      if (!record.ok()) {
        return record.status();
      }
      return inspect::render_reproducibility(record.value(), limits);
    }
    case QueryKind::INVALID:
    case QueryKind::STATUS:
    case QueryKind::LIST_EXPERIMENTS:
    case QueryKind::EXPLAIN_DECISION:
    case QueryKind::VALIDATE_STATE:
    case QueryKind::WORKERS:
      break;
  }
  return make_error(ErrorCode::UNSUPPORTED, ErrorStage::VALIDATION, "query kind is not supported");
}

Result<std::string> Coordinator::explain(ExperimentId experiment, BranchId candidate) const {
  const auto decision = evaluate(experiment, candidate);
  if (!decision.ok()) {
    return decision.status();
  }
  const Limits limits = impl_->limits;
  const std::vector<std::string> lines = inspect::render_decision(decision.value(), limits);
  std::string text;
  for (const std::string& line : lines) {
    text.append(line);
    text.push_back('\n');
  }
  return text;
}

}  // namespace experiment_fabric
