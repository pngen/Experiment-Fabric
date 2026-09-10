// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "coordinator_internal.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

#include "experiment_fabric/hash.hpp"

namespace experiment_fabric {
namespace {

Status invalid_argument(std::string reason) {
  return make_error(ErrorCode::INVALID_ARGUMENT, ErrorStage::VALIDATION, std::move(reason));
}

Status definition_error(std::string reason) {
  return make_error(ErrorCode::INVALID_ARGUMENT, ErrorStage::DEFINITION, std::move(reason));
}

Status not_found(std::string reason) {
  return make_error(ErrorCode::NOT_FOUND, ErrorStage::LIFECYCLE, std::move(reason));
}

Status limits_error(std::string reason) {
  return make_error(ErrorCode::RESOURCE_EXHAUSTED, ErrorStage::LIMITS, std::move(reason));
}

Status lifecycle_error(ErrorCode code, std::string reason) {
  return make_error(code, ErrorStage::LIFECYCLE, std::move(reason));
}

Status validate_spec(const ExperimentSpec& spec, const Limits& limits) {
  if (spec.name.empty() || spec.name.size() > limits.max_name_length) {
    return definition_error("experiment name is empty or too long");
  }
  if (spec.provenance.size() > limits.max_provenance_length) {
    return definition_error("experiment provenance text is too long");
  }
  if (spec.hypothesis_statement.empty() || spec.hypothesis_statement.size() > limits.max_statement_length) {
    return definition_error("hypothesis statement is empty or too long");
  }
  if (spec.metrics.empty()) {
    return definition_error("experiment must declare at least one metric");
  }
  if (spec.metrics.size() > limits.max_metrics_per_experiment) {
    return limits_error("experiment declares more metrics than the configured bound");
  }
  if (spec.branches.empty()) {
    return definition_error("experiment must declare at least one branch");
  }
  if (spec.branches.size() > limits.max_branches_per_experiment) {
    return limits_error("experiment declares more branches than the configured bound");
  }
  std::set<std::string> metric_names;
  for (const MetricSpec& metric : spec.metrics) {
    if (metric.name.empty() || metric.name.size() > limits.max_name_length) {
      return definition_error("metric name is empty or too long");
    }
    if (metric.unit.size() > limits.max_unit_length) {
      return definition_error("metric unit is too long");
    }
    if (!metric_names.insert(metric.name).second) {
      return definition_error("duplicate metric name");
    }
    if (metric.kind == MetricKind::CATEGORICAL && metric.aggregation != AggregationRule::MAJORITY &&
        metric.aggregation != AggregationRule::NONE) {
      return definition_error("categorical metric requires MAJORITY or NONE aggregation");
    }
    if ((metric.kind == MetricKind::REAL || metric.kind == MetricKind::INTEGER) &&
        metric.aggregation == AggregationRule::MAJORITY) {
      return definition_error("numeric metric cannot use MAJORITY aggregation");
    }
    if (metric.lower_bound.has_value() && metric.upper_bound.has_value() &&
        *metric.lower_bound > *metric.upper_bound) {
      return definition_error("metric validity bounds are inverted");
    }
    if (metric.target_low.has_value() && metric.target_high.has_value() &&
        *metric.target_low > *metric.target_high) {
      return definition_error("metric target range is inverted");
    }
  }
  std::set<std::string> branch_names;
  std::uint32_t baseline_count = 0;
  for (const BranchSpec& branch : spec.branches) {
    if (branch.name.empty() || branch.name.size() > limits.max_name_length) {
      return definition_error("branch name is empty or too long");
    }
    if (!branch_names.insert(branch.name).second) {
      return definition_error("duplicate branch name");
    }
    if (branch.role == BranchRole::BASELINE) {
      ++baseline_count;
    }
    if (branch.fork_from.has_value()) {
      bool declared_earlier = false;
      for (const BranchSpec& other : spec.branches) {
        if (&other == &branch) {
          break;
        }
        if (other.name == *branch.fork_from) {
          declared_earlier = true;
          break;
        }
      }
      if (!declared_earlier) {
        return definition_error("branch fork parent must be declared earlier in the specification");
      }
    }
    if (branch.planned_trials == 0 || branch.planned_trials > limits.max_trials_per_branch) {
      return limits_error("branch planned trial count is out of range");
    }
    if (canonical::has_duplicate_parameter_key(branch.parameters)) {
      return definition_error("duplicate branch parameter key");
    }
  }
  if (spec.require_baseline && baseline_count == 0) {
    return definition_error("policy requires a baseline branch but none is designated");
  }
  if (spec.rules.size() > limits.max_comparison_rules) {
    return limits_error("too many comparison rules");
  }
  for (const ComparisonRuleSpec& rule : spec.rules) {
    if (metric_names.find(rule.metric) == metric_names.end()) {
      return definition_error("comparison rule references an unknown metric name");
    }
    if (rule.op == ComparisonOp::TARGET_RANGE && rule.target_low > rule.target_high) {
      return definition_error("comparison target range is inverted");
    }
    if (rule.op == ComparisonOp::MAJORITY_WINS && (rule.threshold < 0.0 || rule.threshold > 1.0)) {
      return definition_error("majority-wins threshold must be a fraction in [0,1]");
    }
  }
  if (canonical::has_duplicate_parameter_key(spec.parameters)) {
    return definition_error("duplicate experiment parameter key");
  }
  if (spec.majority_wins_min_basis_points > 10000) {
    return definition_error("majority-wins basis points out of range");
  }
  if (spec.max_attempts_per_trial == 0 || spec.max_attempts_per_trial > limits.max_attempts_per_trial) {
    return limits_error("max attempts per trial is out of range");
  }
  if (spec.require_baseline && spec.branches.size() < 2) {
    return definition_error("baseline comparison requires a baseline and at least one candidate branch");
  }
  return Status::success();
}

}  // namespace

std::uint64_t derive_trial_seed(std::uint64_t experiment_seed, std::uint64_t ordinal) noexcept {
  DeterministicRng rng(experiment_seed ^ (ordinal * 0x9E3779B97F4A7C15ull));
  return rng.next_u64();
}

// ---------------------------------------------------------------------------
// Identity allocation
// ---------------------------------------------------------------------------

ExperimentId Coordinator::Impl::alloc_experiment_id() { return ExperimentId::from_value(++state.id_watermark_experiment); }
HypothesisId Coordinator::Impl::alloc_hypothesis_id() { return HypothesisId::from_value(++state.id_watermark_hypothesis); }
BranchId Coordinator::Impl::alloc_branch_id() { return BranchId::from_value(++state.id_watermark_branch); }
TrialId Coordinator::Impl::alloc_trial_id() { return TrialId::from_value(++state.id_watermark_trial); }
TrialAttemptId Coordinator::Impl::alloc_attempt_id() { return TrialAttemptId::from_value(++state.id_watermark_attempt); }
ObservationId Coordinator::Impl::alloc_observation_id() {
  return ObservationId::from_value(++state.id_watermark_observation);
}
ArtifactId Coordinator::Impl::alloc_artifact_id() { return ArtifactId::from_value(++state.id_watermark_artifact); }
DecisionId Coordinator::Impl::alloc_decision_id() { return DecisionId::from_value(++state.id_watermark_decision); }
RollbackPointId Coordinator::Impl::alloc_rollback_id() {
  return RollbackPointId::from_value(++state.id_watermark_rollback);
}

std::uint64_t Coordinator::Impl::next_sequence() { return ++state.last_sequence; }
std::uint64_t Coordinator::Impl::current_sequence() const { return state.last_sequence; }

ExperimentRecord* Coordinator::Impl::find_experiment(ExperimentId id) {
  const auto iterator = state.experiments.find(id);
  return iterator == state.experiments.end() ? nullptr : &iterator->second;
}

const ExperimentRecord* Coordinator::Impl::find_experiment(ExperimentId id) const {
  const auto iterator = state.experiments.find(id);
  return iterator == state.experiments.end() ? nullptr : &iterator->second;
}

BranchDefinition* Coordinator::Impl::find_branch(ExperimentRecord& record, BranchId id) {
  for (BranchDefinition& branch : record.definition.branches) {
    if (branch.id == id) {
      return &branch;
    }
  }
  return nullptr;
}

const BranchDefinition* Coordinator::Impl::find_branch(const ExperimentRecord& record, BranchId id) const {
  for (const BranchDefinition& branch : record.definition.branches) {
    if (branch.id == id) {
      return &branch;
    }
  }
  return nullptr;
}

const MetricDefinition* Coordinator::Impl::find_metric(const ExperimentRecord& record, MetricId id) const {
  for (const MetricDefinition& metric : record.definition.metrics) {
    if (metric.id == id) {
      return &metric;
    }
  }
  return nullptr;
}

LiveWorker* Coordinator::Impl::find_live_worker(WorkerId worker, WorkerBootId boot) {
  const auto iterator = live_workers.find({worker.value(), boot.value()});
  if (iterator == live_workers.end()) {
    return nullptr;
  }
  return &iterator->second;
}

void Coordinator::Impl::rebuild_indexes() {
  observation_index.clear();
  artifact_index.clear();
  for (auto& entry : state.experiments) {
    auto& observations = observation_index[entry.first];
    for (std::size_t index = 0; index < entry.second.observations.size(); ++index) {
      observations[entry.second.observations[index].id] = index;
    }
    auto& artifacts = artifact_index[entry.first];
    for (std::size_t index = 0; index < entry.second.artifacts.size(); ++index) {
      artifacts[entry.second.artifacts[index].id] = index;
    }
  }
}

void Coordinator::Impl::log(ExperimentRecord& record, std::string line) {
  if (record.lifecycle_log.size() >= 65536) {
    record.lifecycle_log.erase(record.lifecycle_log.begin());
  }
  record.lifecycle_log.push_back(std::move(line));
}

Status Coordinator::Impl::persist_locked() { return persist_locked(true); }

Status Coordinator::Impl::persist_locked(bool required) {
  if (config.state_path.empty()) {
    return Status::success();
  }
  if (!required && !config.persist_on_mutation) {
    return Status::success();
  }
  auto image = persistence::serialize(state, limits);
  if (!image.ok()) {
    persistence_degraded = true;
    return image.status();
  }
  const Status written = persistence::save(config.state_path, state, limits);
  if (!written.ok()) {
    persistence_degraded = true;
    return written;
  }
  persistence_degraded = false;
  return Status::success();
}

void Coordinator::Impl::notify_event(const std::string& event) const {
  if (config.observer != nullptr) {
    config.observer->on_event(event);
  }
}

void Coordinator::Impl::notify_decision(const Decision& decision) const {
  if (config.observer != nullptr) {
    config.observer->on_decision(decision);
  }
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

Coordinator::Coordinator(Config config) : impl_(std::make_unique<Impl>(std::move(config), Limits{})) {
  impl_->limits = impl_->config.limits;
}

Coordinator::~Coordinator() { (void)stop_server(); }

Result<std::unique_ptr<Coordinator>> Coordinator::create(Config config) {
  if (!Limits::is_coherent(config.limits)) {
    return invalid_argument("limits configuration is not coherent");
  }
  std::unique_ptr<Coordinator> coordinator(new Coordinator(std::move(config)));
  Impl& impl = *coordinator->impl_;

  impl.state = CoordinatorState{};
  impl.state.format_version = kPersistenceFormatVersion;

  if (!impl.config.state_path.empty()) {
    const auto discarded = persistence::discard_temporary_images(impl.config.state_path);
    if (!discarded.ok()) {
      return discarded.status();
    }
    std::error_code error;
    if (std::filesystem::exists(impl.config.state_path, error)) {
      auto loaded = persistence::load(impl.config.state_path, impl.limits);
      if (!loaded.ok()) {
        return loaded.status();
      }
      impl.state = std::move(loaded.value());
    }
  }

  // A fresh coordinator epoch is minted for every process start. The durable
  // high-water mark guarantees epochs strictly increase even if the clock or a
  // seeded value is unreliable, so a retired epoch can never be re-issued.
  std::uint64_t next_epoch = impl.state.last_epoch + 1;
  if (impl.config.epoch_seed != 0 && impl.config.epoch_seed > next_epoch) {
    next_epoch = impl.config.epoch_seed;
  }
  if (next_epoch == 0) {
    next_epoch = 1;
  }
  impl.epoch = CoordinatorEpoch::from_value(next_epoch);
  impl.state.last_epoch = next_epoch;

  impl.rebuild_indexes();

  const Status recovered = coordinator->reconcile_recovered_state();
  if (!recovered.ok()) {
    return recovered;
  }
  return coordinator;
}

CoordinatorEpoch Coordinator::epoch() const noexcept { return impl_->epoch; }

std::uint64_t Coordinator::sequence() const noexcept {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex);
  return impl_->current_sequence();
}

const Limits& Coordinator::limits() const noexcept { return impl_->limits; }

bool Coordinator::server_running() const noexcept { return impl_->server_active.load(); }

Status Coordinator::reconcile_recovered_state() {
  Impl& impl = *impl_;
  bool changed = false;
  {
    std::unique_lock<std::shared_mutex> lock(impl.mutex);
    for (auto& entry : impl.state.experiments) {
      ExperimentRecord& record = entry.second;
      if (record.state == ExperimentState::RECOVERING) {
        record.state = ExperimentState::OPEN;
        changed = true;
      }
      bool touched = false;
      for (auto& attempt_entry : record.attempts) {
        AttemptRecord& attempt = attempt_entry.second;
        if (is_attempt_terminal(attempt.state)) {
          continue;
        }
        attempt.state = AttemptState::REVALIDATION_REQUIRED;
        attempt.failure_kind = FailureKind::PRODUCER_LOSS;
        attempt.failure_detail = "coordinator restart: live process authority was not inherited";
        attempt.updated_sequence = impl.next_sequence();
        touched = true;
        changed = true;
      }
      for (auto& trial_entry : record.trials) {
        TrialRecord& trial = trial_entry.second;
        if (is_terminal(trial.state)) {
          continue;
        }
        impl.log(record, std::string("recovery:trial ") + trial.id.to_string() +
                              " moved from " + std::string(to_string(trial.state)) +
                              " to REVALIDATION_REQUIRED");
        trial.state = TrialState::REVALIDATION_REQUIRED;
        trial.updated_sequence = impl.next_sequence();
        touched = true;
        changed = true;
      }
      if (touched) {
        // Durable conservative intermediate state is written before any
        // resolution so that a crash during recovery cannot invent progress.
        const Status staged = impl.persist_locked();
        if (!staged.ok()) {
          return staged;
        }
      }
    }
  }

  if (changed) {
    std::unique_lock<std::shared_mutex> lock(impl.mutex);
    for (auto& entry : impl.state.experiments) {
      ExperimentRecord& record = entry.second;
      for (auto& trial_entry : record.trials) {
        TrialRecord& trial = trial_entry.second;
        if (trial.state != TrialState::REVALIDATION_REQUIRED) {
          continue;
        }
        const BranchDefinition* branch = impl.find_branch(record, trial.branch);
        const bool attempts_remain = trial.attempt_count < record.definition.max_attempts_per_trial;
        const bool branch_eligible = branch != nullptr && is_branch_eligible(branch->state) &&
                                     branch->generation == trial.branch_generation;
        if (attempts_remain && branch_eligible && is_experiment_mutable(record.state) &&
            trial.generation == record.definition.generation) {
          trial.state = TrialState::READY;
          trial.authoritative_attempt = TrialAttemptId{};
          trial.next_attempt_number = TrialAttemptNumber::from_value(trial.attempt_count + 1);
          trial.failure_kind = FailureKind::PRODUCER_LOSS;
          trial.failure_detail = "recovered: fresh attempt required before the trial can complete";
          impl.log(record, "recovery:trial " + trial.id.to_string() + " resolved to READY");
        } else {
          trial.state = TrialState::FAILED;
          trial.failure_kind = FailureKind::PRODUCER_LOSS;
          trial.failure_detail = "recovered: no authoritative completion and no retry budget remains";
          impl.log(record, "recovery:trial " + trial.id.to_string() + " resolved to FAILED");
        }
        // Every attempt that survived the crash is resolved to a terminal
        // conservative state, so no recovered dynamic authority stays live.
        for (auto& attempt_entry : record.attempts) {
          AttemptRecord& attempt = attempt_entry.second;
          if (attempt.trial != trial.id || is_attempt_terminal(attempt.state)) {
            continue;
          }
          attempt.state = AttemptState::ABANDONED;
          attempt.failure_kind = FailureKind::PRODUCER_LOSS;
          attempt.failure_detail = "recovered: the previous process incarnation holds no authority";
          attempt.updated_sequence = impl.next_sequence();
        }
        trial.updated_sequence = impl.next_sequence();
      }
    }
    const Status persisted = impl.persist_locked();
    if (!persisted.ok()) {
      return persisted;
    }
  }
  return Status::success();
}

// ---------------------------------------------------------------------------
// Definition operations
// ---------------------------------------------------------------------------

Result<ExperimentId> Coordinator::create_experiment(const ExperimentSpec& spec) {
  Impl& impl = *impl_;
  const Status valid = validate_spec(spec, impl.limits);
  if (!valid.ok()) {
    return valid;
  }

  ExperimentId experiment;
  {
    std::unique_lock<std::shared_mutex> lock(impl.mutex);
    if (!impl.admitting_work) {
      return make_error(ErrorCode::SHUTTING_DOWN, ErrorStage::SHUTDOWN, "coordinator is shutting down");
    }
    if (impl.state.experiments.size() >= impl.limits.max_experiments) {
      return limits_error("too many experiments");
    }
    experiment = impl.alloc_experiment_id();
    if (impl.state.experiments.find(experiment) != impl.state.experiments.end()) {
      return make_error(ErrorCode::ALREADY_EXISTS, ErrorStage::IDENTITY, "experiment identity collision");
    }

    ExperimentRecord record;
    const std::uint64_t sequence = impl.next_sequence();
    record.created_sequence = sequence;
    record.revision_sequence = sequence;
    record.state = ExperimentState::OPEN;

    ExperimentDefinition& definition = record.definition;
    definition.id = experiment;
    definition.generation = ExperimentGeneration::from_value(1);
    definition.name = spec.name;
    definition.hypothesis = impl.alloc_hypothesis_id();
    definition.hypothesis_revision = HypothesisRevision::from_value(1);
    definition.provenance = spec.provenance;
    definition.seed_policy = spec.seed_policy;
    definition.experiment_seed = spec.experiment_seed;
    definition.retry_policy = spec.retry_policy;
    definition.max_attempts_per_trial = spec.max_attempts_per_trial;
    definition.partial_failure_policy = spec.partial_failure_policy;
    definition.late_evidence_policy = spec.late_evidence_policy;
    definition.parameters = canonical::normalize_parameters(spec.parameters);
    definition.created_sequence = sequence;

    record.hypothesis.id = definition.hypothesis;
    record.hypothesis.revision = definition.hypothesis_revision;
    record.hypothesis.statement = spec.hypothesis_statement;
    record.hypothesis.expected_direction = spec.expected_direction;
    record.hypothesis.provenance = spec.provenance;
    record.hypothesis.created_sequence = sequence;

    std::map<std::string, MetricId> metric_ids;
    for (const MetricSpec& metric_spec : spec.metrics) {
      MetricDefinition metric;
      metric.id = MetricId::from_value(++impl.state.id_watermark_metric);
      metric.name = metric_spec.name;
      metric.unit = metric_spec.unit;
      metric.kind = metric_spec.kind;
      metric.direction = metric_spec.direction;
      metric.aggregation = metric_spec.aggregation;
      metric.required = metric_spec.required;
      metric.accepts_non_finite = metric_spec.accepts_non_finite;
      metric.lower_bound = metric_spec.lower_bound;
      metric.upper_bound = metric_spec.upper_bound;
      metric.target_low = metric_spec.target_low;
      metric.target_high = metric_spec.target_high;
      metric.weight = metric_spec.weight;
      metric.priority = metric_spec.priority;
      metric.categories = metric_spec.categories;
      metric_ids.emplace(metric.name, metric.id);
      definition.metrics.push_back(std::move(metric));
    }
    if (spec.expected_metric.has_value()) {
      const auto iterator = metric_ids.find(*spec.expected_metric);
      if (iterator == metric_ids.end()) {
        return definition_error("hypothesis references an unknown expected metric name");
      }
      record.hypothesis.expected_metric = iterator->second;
    }

    ComparisonPolicy& policy = definition.policy;
    policy.id = PolicyId::from_value(++impl.state.id_watermark_policy);
    policy.generation = PolicyGeneration::from_value(1);
    policy.require_baseline = spec.require_baseline;
    policy.min_completed_trials_per_branch = spec.min_completed_trials_per_branch;
    policy.min_valid_observations_per_required_metric = spec.min_valid_observations_per_required_metric;
    policy.reject_on_any_invalid_observation = spec.reject_on_any_invalid_observation;
    policy.require_environment_match = spec.require_environment_match;
    policy.majority_wins_min_basis_points = spec.majority_wins_min_basis_points;
    policy.use_weighted_score = spec.use_weighted_score;
    policy.accept_score_threshold = spec.accept_score_threshold;
    for (const ComparisonRuleSpec& rule_spec : spec.rules) {
      ComparisonRule rule;
      rule.metric = metric_ids.at(rule_spec.metric);
      rule.op = rule_spec.op;
      rule.threshold = rule_spec.threshold;
      rule.target_low = rule_spec.target_low;
      rule.target_high = rule_spec.target_high;
      rule.priority = rule_spec.priority;
      policy.rules.push_back(rule);
    }

    std::map<std::string, BranchId> branch_ids;
    std::map<std::string, const BranchSpec*> branch_specs;
    for (const BranchSpec& branch_spec : spec.branches) {
      BranchDefinition branch;
      branch.id = impl.alloc_branch_id();
      branch.name = branch_spec.name;
      branch.role = branch_spec.role;
      branch.generation = BranchGeneration::from_value(1);
      branch.state = BranchState::ACTIVE;
      branch.parameters = canonical::normalize_parameters(branch_spec.parameters);
      if (branch_spec.input_set.has_value()) {
        branch.input_set = InputSetId::from_value(fnv1a64(*branch_spec.input_set));
        branch.input_set_present = true;
      }
      if (branch_spec.environment.has_value()) {
        branch.environment = EnvironmentId::from_value(fnv1a64(*branch_spec.environment));
        branch.environment_present = true;
      }
      branch.environment_fingerprint = branch_spec.environment_fingerprint;
      branch.seed = branch_spec.seed;
      branch.planned_trials = branch_spec.planned_trials;
      branch.created_sequence = sequence;
      if (branch_spec.fork_from.has_value()) {
        const auto parent = branch_ids.find(*branch_spec.fork_from);
        if (parent == branch_ids.end()) {
          return definition_error("branch fork parent must be declared earlier in the specification");
        }
        const BranchSpec* parent_spec = branch_specs.at(*branch_spec.fork_from);
        branch.parent = parent->second;
        branch.forked = true;
        std::vector<ParameterAssignment> merged = canonical::normalize_parameters(parent_spec->parameters);
        for (const ParameterAssignment& assignment : branch.parameters) {
          merged.erase(std::remove_if(merged.begin(), merged.end(),
                                      [&assignment](const ParameterAssignment& candidate) {
                                        return candidate.key == assignment.key;
                                      }),
                       merged.end());
          merged.push_back(assignment);
        }
        branch.parameters = canonical::normalize_parameters(merged);
        if (!branch.input_set_present && parent_spec->input_set.has_value()) {
          branch.input_set = InputSetId::from_value(fnv1a64(*parent_spec->input_set));
          branch.input_set_present = true;
        }
        if (!branch.environment_present && parent_spec->environment.has_value()) {
          branch.environment = EnvironmentId::from_value(fnv1a64(*parent_spec->environment));
          branch.environment_present = true;
        }
        if (branch.environment_fingerprint.empty()) {
          branch.environment_fingerprint = parent_spec->environment_fingerprint;
        }
      }
      branch_ids.emplace(branch.name, branch.id);
      branch_specs.emplace(branch.name, &branch_spec);
      definition.branches.push_back(std::move(branch));
    }

    impl.log(record, "experiment-created generation=" + definition.generation.to_string());
    impl.state.experiments.emplace(experiment, std::move(record));

    // The stored record is validated, not the moved-from local.
    const auto stored = impl.state.experiments.find(experiment);
    const Status validated =
        stored == impl.state.experiments.end()
            ? make_error(ErrorCode::INTERNAL, ErrorStage::IDENTITY, "the new experiment record was not stored")
            : validate_experiment_record(stored->second, impl.limits, experiment);
    if (!validated.ok()) {
      impl.state.experiments.erase(experiment);
      return validated;
    }
    const Status persisted = impl.persist_locked();
    if (!persisted.ok()) {
      impl.state.experiments.erase(experiment);
      return persisted;
    }
  }
  impl.notify_event("experiment-created:" + experiment.to_string());
  return experiment;
}

void Coordinator::Impl::abandon_stale_live_work(ExperimentRecord& record, ExperimentGeneration generation,
                                                std::uint64_t sequence) {
  (void)generation;
  for (auto& attempt_entry : record.attempts) {
    AttemptRecord& attempt = attempt_entry.second;
    if (is_attempt_terminal(attempt.state)) {
      continue;
    }
    attempt.state = AttemptState::ABANDONED;
    attempt.failure_kind = FailureKind::EXPERIMENT_INVALIDATION;
    attempt.failure_detail = "superseded by an experiment generation change";
    attempt.updated_sequence = sequence;
  }
  for (auto& trial_entry : record.trials) {
    TrialRecord& trial = trial_entry.second;
    if (is_terminal(trial.state)) {
      continue;
    }
    trial.state = TrialState::SUPERSEDED;
    trial.failure_kind = FailureKind::EXPERIMENT_INVALIDATION;
    trial.failure_detail = "superseded by an experiment generation change";
    trial.updated_sequence = sequence;
  }
}

Result<HypothesisRevision> Coordinator::revise_hypothesis(ExperimentId experiment,
                                                          const HypothesisRevisionSpec& spec) {
  Impl& impl = *impl_;
  HypothesisRevision revision;
  {
    std::unique_lock<std::shared_mutex> lock(impl.mutex);
    ExperimentRecord* record = impl.find_experiment(experiment);
    if (record == nullptr) {
      return not_found("experiment does not exist");
    }
    if (!is_experiment_mutable(record->state)) {
      return lifecycle_error(ErrorCode::INVALID_TRANSITION, "experiment does not accept new hypothesis revisions");
    }
    if (spec.statement.empty() || spec.statement.size() > impl.limits.max_statement_length) {
      return definition_error("hypothesis statement is empty or too long");
    }
    if (spec.expected_metric.valid() && impl.find_metric(*record, spec.expected_metric) == nullptr) {
      return definition_error("hypothesis references an unknown metric");
    }
    const std::uint64_t sequence = impl.next_sequence();
    Hypothesis superseded = record->hypothesis;
    superseded.superseded = true;
    superseded.superseded_by_revision = HypothesisRevision::from_value(record->hypothesis.revision.value() + 1);
    record->hypothesis_history.push_back(superseded);

    record->hypothesis.revision = HypothesisRevision::from_value(record->hypothesis.revision.value() + 1);
    record->hypothesis.statement = spec.statement;
    record->hypothesis.expected_direction = spec.expected_direction;
    record->hypothesis.expected_metric = spec.expected_metric;
    record->hypothesis.provenance = spec.provenance;
    record->hypothesis.created_sequence = sequence;
    record->hypothesis.superseded = false;
    record->hypothesis.superseded_by_revision = HypothesisRevision{};
    record->definition.hypothesis_revision = record->hypothesis.revision;

    // A hypothesis revision changes experiment semantics: the experiment
    // generation is incremented so that every trial derived from the previous
    // revision is fenced out of current authority.
    record->definition.generation = ExperimentGeneration::from_value(record->definition.generation.value() + 1);
    record->revision_sequence = sequence;
    impl.abandon_stale_live_work(*record, record->definition.generation, sequence);
    impl.log(*record, "hypothesis-revised revision=" + record->hypothesis.revision.to_string() +
                          " generation=" + record->definition.generation.to_string());
    revision = record->hypothesis.revision;
    const Status persisted = impl.persist_locked();
    if (!persisted.ok()) {
      return persisted;
    }
  }
  impl.notify_event("hypothesis-revised:" + experiment.to_string());
  return revision;
}

Result<BranchId> Coordinator::fork_branch(ExperimentId experiment, BranchId parent, const BranchSpec& spec) {
  Impl& impl = *impl_;
  BranchId branch_id;
  {
    std::unique_lock<std::shared_mutex> lock(impl.mutex);
    ExperimentRecord* record = impl.find_experiment(experiment);
    if (record == nullptr) {
      return not_found("experiment does not exist");
    }
    if (!is_experiment_mutable(record->state)) {
      return lifecycle_error(ErrorCode::INVALID_TRANSITION, "experiment does not accept new branches");
    }
    const BranchDefinition* parent_branch = impl.find_branch(*record, parent);
    if (parent_branch == nullptr) {
      return not_found("parent branch does not exist");
    }
    if (parent_branch->state == BranchState::INVALID || parent_branch->state == BranchState::SUPERSEDED) {
      return lifecycle_error(ErrorCode::STALE_BRANCH, "parent branch is not eligible for forking");
    }
    if (record->definition.branches.size() >= impl.limits.max_branches_per_experiment) {
      return limits_error("too many branches");
    }
    if (spec.name.empty() || spec.name.size() > impl.limits.max_name_length) {
      return definition_error("branch name is empty or too long");
    }
    for (const BranchDefinition& existing : record->definition.branches) {
      if (existing.name == spec.name) {
        return make_error(ErrorCode::ALREADY_EXISTS, ErrorStage::DEFINITION, "branch name already exists");
      }
    }
    if (spec.planned_trials == 0 || spec.planned_trials > impl.limits.max_trials_per_branch) {
      return limits_error("branch planned trial count is out of range");
    }
    if (canonical::has_duplicate_parameter_key(spec.parameters)) {
      return definition_error("duplicate branch parameter key");
    }
    for (const BranchDefinition& existing : record->definition.branches) {
      if (existing.role == BranchRole::BASELINE && spec.role == BranchRole::BASELINE) {
        return definition_error("an experiment generation may designate only one baseline branch");
      }
    }

    const std::uint64_t sequence = impl.next_sequence();
    BranchDefinition branch;
    branch.id = impl.alloc_branch_id();
    branch.name = spec.name;
    branch.role = spec.role;
    branch.parent = parent;
    branch.generation = BranchGeneration::from_value(1);
    branch.state = BranchState::ACTIVE;
    branch.created_sequence = sequence;
    branch.forked = true;
    branch.planned_trials = spec.planned_trials;
    branch.parameters = canonical::normalize_parameters(parent_branch->parameters);
    for (const ParameterAssignment& assignment : canonical::normalize_parameters(spec.parameters)) {
      branch.parameters.erase(std::remove_if(branch.parameters.begin(), branch.parameters.end(),
                                             [&assignment](const ParameterAssignment& candidate) {
                                               return candidate.key == assignment.key;
                                             }),
                              branch.parameters.end());
      branch.parameters.push_back(assignment);
    }
    branch.parameters = canonical::normalize_parameters(branch.parameters);
    branch.input_set = parent_branch->input_set;
    branch.input_set_present = parent_branch->input_set_present;
    branch.environment = parent_branch->environment;
    branch.environment_present = parent_branch->environment_present;
    branch.environment_fingerprint = parent_branch->environment_fingerprint;
    branch.seed = spec.seed.has_value() ? spec.seed : parent_branch->seed;

    record->definition.branches.push_back(branch);
    branch_id = branch.id;
    record->revision_sequence = sequence;
    impl.log(*record, "branch-forked branch=" + branch_id.to_string() + " parent=" + parent.to_string());
    const Status validated = validate_experiment_record(*record, impl.limits, experiment);
    if (!validated.ok()) {
      record->definition.branches.pop_back();
      return validated;
    }
    const Status persisted = impl.persist_locked();
    if (!persisted.ok()) {
      record->definition.branches.pop_back();
      return persisted;
    }
  }
  impl.notify_event("branch-forked:" + branch_id.to_string());
  return branch_id;
}

Result<BranchId> Coordinator::retire_branch(ExperimentId experiment, BranchId branch_id, std::string reason) {
  Impl& impl = *impl_;
  {
    std::unique_lock<std::shared_mutex> lock(impl.mutex);
    ExperimentRecord* record = impl.find_experiment(experiment);
    if (record == nullptr) {
      return not_found("experiment does not exist");
    }
    BranchDefinition* branch = impl.find_branch(*record, branch_id);
    if (branch == nullptr) {
      return not_found("branch does not exist");
    }
    if (!is_valid_transition(branch->state, BranchState::RETIRED)) {
      return invalid_transition_status("branch", to_string(branch->state), "RETIRED");
    }
    const std::uint64_t sequence = impl.next_sequence();
    branch->state = BranchState::RETIRED;
    // Retiring a branch is a branch-local semantic change: only that branch's
    // generation advances, so unrelated branches keep their evidence.
    branch->generation = BranchGeneration::from_value(branch->generation.value() + 1);
    for (auto& trial_entry : record->trials) {
      TrialRecord& trial = trial_entry.second;
      if (trial.branch != branch_id || is_terminal(trial.state)) {
        continue;
      }
      trial.state = TrialState::SUPERSEDED;
      trial.failure_kind = FailureKind::EXPERIMENT_INVALIDATION;
      trial.failure_detail = "branch retired: " + reason;
      trial.updated_sequence = sequence;
    }
    for (auto& attempt_entry : record->attempts) {
      AttemptRecord& attempt = attempt_entry.second;
      if (attempt.branch != branch_id || is_attempt_terminal(attempt.state)) {
        continue;
      }
      attempt.state = AttemptState::ABANDONED;
      attempt.failure_kind = FailureKind::EXPERIMENT_INVALIDATION;
      attempt.failure_detail = "branch retired: " + reason;
      attempt.updated_sequence = sequence;
    }
    record->revision_sequence = sequence;
    impl.log(*record, "branch-retired branch=" + branch_id.to_string() + " reason=" + reason);
    const Status persisted = impl.persist_locked();
    if (!persisted.ok()) {
      return persisted;
    }
  }
  impl.notify_event("branch-retired:" + branch_id.to_string());
  return branch_id;
}

Result<TrialId> Coordinator::create_trial(ExperimentId experiment, BranchId branch, std::string payload) {
  Impl& impl = *impl_;
  TrialId trial_id;
  {
    std::unique_lock<std::shared_mutex> lock(impl.mutex);
    ExperimentRecord* record = impl.find_experiment(experiment);
    if (record == nullptr) {
      return not_found("experiment does not exist");
    }
    if (!is_experiment_mutable(record->state)) {
      return lifecycle_error(ErrorCode::INVALID_TRANSITION, "experiment does not accept new trials");
    }
    BranchDefinition* branch_definition = impl.find_branch(*record, branch);
    if (branch_definition == nullptr) {
      return not_found("branch does not exist");
    }
    if (!is_branch_eligible(branch_definition->state)) {
      return lifecycle_error(ErrorCode::STALE_BRANCH, "branch is not eligible for new trials");
    }
    if (payload.size() > impl.limits.max_payload_text_length) {
      return limits_error("trial payload exceeds the configured bound");
    }
    std::uint32_t existing = 0;
    for (const auto& entry : record->trials) {
      if (entry.second.branch == branch && entry.second.generation == record->definition.generation) {
        ++existing;
      }
    }
    if (existing >= branch_definition->planned_trials) {
      return limits_error("branch already holds its planned number of trials");
    }
    if (record->trials.size() >= record->definition.branches.size() *
                                      static_cast<std::size_t>(impl.limits.max_trials_per_branch)) {
      return limits_error("experiment holds too many trials");
    }

    const std::uint64_t sequence = impl.next_sequence();
    TrialRecord trial;
    trial.id = impl.alloc_trial_id();
    trial.experiment = experiment;
    trial.generation = record->definition.generation;
    trial.branch = branch;
    trial.branch_generation = branch_definition->generation;
    trial.hypothesis = record->definition.hypothesis;
    trial.hypothesis_revision = record->definition.hypothesis_revision;
    trial.policy = record->definition.policy.id;
    trial.policy_generation = record->definition.policy.generation;
    trial.state = TrialState::READY;
    trial.next_attempt_number = TrialAttemptNumber::from_value(1);
    trial.input_set = branch_definition->input_set;
    trial.input_set_present = branch_definition->input_set_present;
    trial.environment = branch_definition->environment;
    trial.environment_present = branch_definition->environment_present;
    trial.environment_fingerprint = branch_definition->environment_fingerprint;
    trial.payload = std::move(payload);
    trial.created_sequence = sequence;
    trial.updated_sequence = sequence;

    switch (record->definition.seed_policy) {
      case SeedPolicy::FIXED:
        trial.seed = record->definition.experiment_seed;
        break;
      case SeedPolicy::DERIVED_PER_TRIAL: {
        const std::uint64_t base = record->definition.experiment_seed.value_or(0);
        trial.seed = derive_trial_seed(base, trial.id.value());
        break;
      }
      case SeedPolicy::PRODUCER_SUPPLIED:
      case SeedPolicy::NONE:
        break;
    }

    trial_id = trial.id;
    record->trials.emplace(trial_id, std::move(trial));
    impl.log(*record, "trial-created trial=" + trial_id.to_string() + " branch=" + branch.to_string());
    const Status validated = validate_experiment_record(*record, impl.limits, experiment);
    if (!validated.ok()) {
      record->trials.erase(trial_id);
      return validated;
    }
    const Status persisted = impl.persist_locked();
    if (!persisted.ok()) {
      record->trials.erase(trial_id);
      return persisted;
    }
  }
  impl.notify_event("trial-created:" + trial_id.to_string());
  return trial_id;
}

// ---------------------------------------------------------------------------
// Evidence
// ---------------------------------------------------------------------------

std::vector<const TrialRecord*> Coordinator::Impl::authoritative_trials(const ExperimentRecord& record,
                                                                       BranchId branch) const {
  std::vector<const TrialRecord*> trials;
  const BranchDefinition* definition = find_branch(record, branch);
  if (definition == nullptr) {
    return trials;
  }
  for (const auto& entry : record.trials) {
    const TrialRecord& trial = entry.second;
    if (trial.branch != branch || trial.state != TrialState::COMPLETED || trial.committed_attempts != 1) {
      continue;
    }
    if (trial.generation != record.definition.generation) {
      continue;
    }
    if (trial.branch_generation != definition->generation) {
      continue;
    }
    if (trial.hypothesis_revision != record.definition.hypothesis_revision) {
      continue;
    }
    if (trial.policy != record.definition.policy.id ||
        trial.policy_generation != record.definition.policy.generation) {
      continue;
    }
    trials.push_back(&trial);
  }
  std::sort(trials.begin(), trials.end(),
            [](const TrialRecord* lhs, const TrialRecord* rhs) { return lhs->id < rhs->id; });
  return trials;
}

std::vector<BranchEvidence> Coordinator::Impl::compute_evidence(const ExperimentRecord& record) const {
  struct ObservationSummary {
    const Observation* valid = nullptr;
    std::uint32_t invalid = 0;
    std::uint32_t missing = 0;
    std::uint32_t unsupported = 0;
  };
  // Evidence is bound to the authoritative attempt of each trial. Observations
  // left behind by abandoned, failed or superseded attempts are retained as
  // history but never contribute to a current aggregate.
  std::map<std::pair<std::uint64_t, std::uint64_t>, ObservationSummary> index;
  for (const Observation& observation : record.observations) {
    const auto trial_iterator = record.trials.find(observation.trial);
    if (trial_iterator == record.trials.end()) {
      continue;
    }
    if (observation.attempt != trial_iterator->second.authoritative_attempt) {
      continue;
    }
    ObservationSummary& summary = index[{observation.trial.value(), observation.metric.value()}];
    switch (observation.validity) {
      case ObservationValidity::VALID:
        if (summary.valid == nullptr) {
          summary.valid = &observation;
        }
        break;
      case ObservationValidity::INVALID:
        ++summary.invalid;
        break;
      case ObservationValidity::MISSING:
        ++summary.missing;
        break;
      case ObservationValidity::UNSUPPORTED:
        ++summary.unsupported;
        break;
    }
  }

  std::vector<BranchDefinition> branches = record.definition.branches;
  std::sort(branches.begin(), branches.end(),
            [](const BranchDefinition& lhs, const BranchDefinition& rhs) { return lhs.id < rhs.id; });
  std::vector<MetricDefinition> metrics = record.definition.metrics;
  std::sort(metrics.begin(), metrics.end(),
            [](const MetricDefinition& lhs, const MetricDefinition& rhs) { return lhs.id < rhs.id; });

  std::vector<BranchEvidence> evidence;
  for (const BranchDefinition& branch : branches) {
    BranchEvidence summary;
    summary.branch = branch.id;
    summary.generation = branch.generation;
    summary.role = branch.role;
    summary.state = branch.state;
    summary.planned_trials = branch.planned_trials;
    for (const auto& entry : record.trials) {
      const TrialRecord& trial = entry.second;
      if (trial.branch != branch.id) {
        continue;
      }
      switch (trial.state) {
        case TrialState::COMPLETED:
          break;
        case TrialState::FAILED:
          ++summary.failed_trials;
          break;
        case TrialState::CANCELLED:
          ++summary.cancelled_trials;
          break;
        case TrialState::INVALID:
          ++summary.invalid_trials;
          break;
        case TrialState::SUPERSEDED:
          ++summary.superseded_trials;
          break;
        default:
          ++summary.open_trials;
          break;
      }
    }
    const std::vector<const TrialRecord*> trials = authoritative_trials(record, branch.id);
    summary.completed_trials = static_cast<std::uint32_t>(trials.size());

    for (const MetricDefinition& metric : metrics) {
      MetricAggregate aggregate;
      aggregate.metric = metric.id;
      aggregate.rule = metric.aggregation;
      std::vector<double> values;
      std::vector<std::string> order;
      std::map<std::string, std::uint32_t> categories;
      for (const TrialRecord* trial : trials) {
        order.push_back(trial->id.to_string());
        const auto iterator = index.find({trial->id.value(), metric.id.value()});
        if (iterator == index.end()) {
          ++aggregate.missing_count;
          continue;
        }
        const ObservationSummary& observation = iterator->second;
        aggregate.invalid_count += observation.invalid;
        aggregate.missing_count += observation.missing;
        aggregate.unsupported_count += observation.unsupported;
        if (observation.valid == nullptr) {
          ++aggregate.missing_count;
          continue;
        }
        ++aggregate.valid_count;
        switch (observation.valid->value.kind) {
          case MetricKind::REAL:
            values.push_back(observation.valid->value.real);
            break;
          case MetricKind::INTEGER:
            values.push_back(static_cast<double>(observation.valid->value.integer));
            break;
          case MetricKind::BOOLEAN:
            values.push_back(observation.valid->value.boolean ? 1.0 : 0.0);
            break;
          case MetricKind::CATEGORICAL:
            categories[observation.valid->value.category] += 1;
            break;
        }
      }
      aggregate.trial_order = order;
      aggregate.raw_values = values;

      const bool categorical = metric.kind == MetricKind::CATEGORICAL || metric.aggregation == AggregationRule::MAJORITY;
      if (categorical) {
        std::string best;
        std::uint32_t best_count = 0;
        for (const auto& entry : categories) {
          if (entry.second > best_count || (entry.second == best_count && entry.first < best)) {
            best = entry.first;
            best_count = entry.second;
          }
        }
        if (best_count > 0) {
          aggregate.present = true;
          aggregate.category = best;
        }
      } else if (!values.empty() && metric.aggregation != AggregationRule::NONE) {
        aggregate.present = true;
        switch (metric.aggregation) {
          case AggregationRule::MEAN: {
            double total = 0.0;
            for (const double value : values) {
              total += value;
            }
            aggregate.value = total / static_cast<double>(values.size());
            break;
          }
          case AggregationRule::MEDIAN: {
            std::vector<double> sorted = values;
            std::sort(sorted.begin(), sorted.end());
            const std::size_t middle = sorted.size() / 2;
            aggregate.value =
                sorted.size() % 2 == 0 ? (sorted[middle - 1] + sorted[middle]) / 2.0 : sorted[middle];
            break;
          }
          case AggregationRule::MIN:
            aggregate.value = *std::min_element(values.begin(), values.end());
            break;
          case AggregationRule::MAX:
            aggregate.value = *std::max_element(values.begin(), values.end());
            break;
          case AggregationRule::SUM: {
            double total = 0.0;
            for (const double value : values) {
              total += value;
            }
            aggregate.value = total;
            break;
          }
          default:
            aggregate.value = values.front();
            break;
        }
      } else if (!values.empty()) {
        aggregate.present = true;
        aggregate.value = values.front();
      }
      summary.aggregates.push_back(std::move(aggregate));
    }
    evidence.push_back(std::move(summary));
  }
  return evidence;
}

// ---------------------------------------------------------------------------
// Persistence and validation
// ---------------------------------------------------------------------------

Status Coordinator::save() {
  Impl& impl = *impl_;
  std::unique_lock<std::shared_mutex> lock(impl.mutex);
  return impl.persist_locked();
}

Status Coordinator::validate_state() const {
  Impl& impl = *impl_;
  std::shared_lock<std::shared_mutex> lock(impl.mutex);
  return impl.state.validate(impl.limits);
}

Result<std::string> Coordinator::state_digest_hex() const {
  Impl& impl = *impl_;
  std::shared_lock<std::shared_mutex> lock(impl.mutex);
  Sha256 hasher;
  hasher.update_length_prefixed("coordinator-state-v1");
  hasher.update_u64(impl.state.last_sequence);
  for (const auto& entry : impl.state.experiments) {
    hasher.update(impl.state.experiment_digest(entry.first));
  }
  return to_hex(hasher.finalize());
}

}  // namespace experiment_fabric
