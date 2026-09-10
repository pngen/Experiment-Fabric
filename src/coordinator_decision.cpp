// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

#include "coordinator_internal.hpp"

namespace experiment_fabric {
namespace {

Status lifecycle_error(ErrorCode code, std::string reason) {
  return make_error(code, ErrorStage::LIFECYCLE, std::move(reason));
}

Status decision_error(std::string reason) {
  return make_error(ErrorCode::INSUFFICIENT_EVIDENCE, ErrorStage::DECISION, std::move(reason));
}

std::string metric_name(const ExperimentRecord& record, MetricId id) {
  for (const MetricDefinition& metric : record.definition.metrics) {
    if (metric.id == id) {
      return metric.name;
    }
  }
  return std::string("metric#") + id.to_string();
}

/// Signed improvement of candidate over baseline in the direction the metric
/// declares. Positive always means "better".
double improvement(MetricDirection direction, double baseline, double candidate) {
  switch (direction) {
    case MetricDirection::MINIMIZE:
      return baseline - candidate;
    case MetricDirection::MAXIMIZE:
      return candidate - baseline;
    case MetricDirection::TARGET:
    case MetricDirection::INFORMATIONAL:
      return candidate - baseline;
  }
  return candidate - baseline;
}

bool branch_has_metric(const BranchEvidence& evidence, MetricId metric, const MetricAggregate** out) {
  for (const MetricAggregate& aggregate : evidence.aggregates) {
    if (aggregate.metric == metric) {
      if (out != nullptr) {
        *out = &aggregate;
      }
      return true;
    }
  }
  if (out != nullptr) {
    *out = nullptr;
  }
  return false;
}

}  // namespace

void Coordinator::Impl::apply_constraints(const ExperimentRecord& record, const BranchDefinition& candidate,
                                          const BranchDefinition* baseline,
                                          const std::vector<BranchEvidence>& evidence,
                                          std::vector<ConstraintResult>& constraints, DecisionOutcome& outcome,
                                          FailureKind& reason_kind, std::string& reason) const {
  const ComparisonPolicy& policy = record.definition.policy;
  constraints.clear();
  outcome = DecisionOutcome::ACCEPT;
  reason_kind = FailureKind::NONE;
  reason = "all hard evidence constraints passed and every decisive comparison factor was satisfied";

  const auto add = [&constraints](std::string name, bool passed, std::string detail) {
    constraints.push_back(ConstraintResult{std::move(name), passed, std::move(detail)});
  };
  const auto fail = [&outcome, &reason_kind, &reason](DecisionOutcome new_outcome, FailureKind kind,
                                                      std::string new_reason) {
    if (outcome == DecisionOutcome::ACCEPT) {
      outcome = new_outcome;
      reason_kind = kind;
      reason = std::move(new_reason);
    }
  };

  add("experiment_open", record.state == ExperimentState::OPEN,
      std::string("experiment state is ") + std::string(to_string(record.state)));
  if (record.state != ExperimentState::OPEN) {
    switch (record.state) {
      case ExperimentState::CANCELLED:
        fail(DecisionOutcome::CANCELLED, FailureKind::CANCELLATION, "the experiment was cancelled");
        break;
      case ExperimentState::SUPERSEDED:
        fail(DecisionOutcome::SUPERSEDED, FailureKind::EXPERIMENT_INVALIDATION,
             "the experiment was superseded");
        break;
      case ExperimentState::RECOVERING:
        fail(DecisionOutcome::INSUFFICIENT_EVIDENCE, FailureKind::PRODUCER_LOSS,
             "the experiment is recovering and has not reconciled its live authority");
        break;
      case ExperimentState::FINALIZED:
        fail(DecisionOutcome::INVALID, FailureKind::POLICY_REJECTION,
             "the experiment already holds an authoritative decision");
        break;
      case ExperimentState::OPEN:
        break;
    }
  }

  const BranchEvidence* candidate_evidence = nullptr;
  const BranchEvidence* baseline_evidence = nullptr;
  for (const BranchEvidence& entry : evidence) {
    if (entry.branch == candidate.id) {
      candidate_evidence = &entry;
    }
    if (baseline != nullptr && entry.branch == baseline->id) {
      baseline_evidence = &entry;
    }
  }

  add("candidate_branch_eligible", candidate_evidence != nullptr && is_branch_eligible(candidate.state) &&
                                      candidate_evidence->generation == candidate.generation,
      std::string("candidate branch state is ") + std::string(to_string(candidate.state)));
  if (candidate_evidence == nullptr || !is_branch_eligible(candidate.state)) {
    if (candidate.state == BranchState::INVALID) {
      fail(DecisionOutcome::INVALID, FailureKind::POLICY_REJECTION, "the candidate branch is invalid");
    } else if (candidate.state == BranchState::SUPERSEDED) {
      fail(DecisionOutcome::SUPERSEDED, FailureKind::EXPERIMENT_INVALIDATION,
           "the candidate branch was superseded");
    } else {
      fail(DecisionOutcome::INSUFFICIENT_EVIDENCE, FailureKind::INSUFFICIENT_EVIDENCE,
           "the candidate branch is not eligible for a decision");
    }
  }

  add("baseline_present", !policy.require_baseline || baseline != nullptr,
      baseline == nullptr ? std::string("no baseline branch is designated") : std::string("baseline designated"));
  if (policy.require_baseline && baseline == nullptr) {
    fail(DecisionOutcome::INSUFFICIENT_EVIDENCE, FailureKind::INSUFFICIENT_EVIDENCE,
         "the policy requires a baseline branch and none is designated");
  } else if (baseline != nullptr) {
    add("baseline_branch_eligible", baseline_evidence != nullptr && is_branch_eligible(baseline->state),
        std::string("baseline branch state is ") + std::string(to_string(baseline->state)));
    if (baseline_evidence == nullptr || !is_branch_eligible(baseline->state)) {
      fail(DecisionOutcome::INSUFFICIENT_EVIDENCE, FailureKind::INSUFFICIENT_EVIDENCE,
           "the baseline branch is not eligible for a decision");
    }
  }

  if (candidate_evidence != nullptr) {
    const bool enough = candidate_evidence->completed_trials >= policy.min_completed_trials_per_branch;
    add("candidate_completed_trials", enough,
        "completed=" + std::to_string(candidate_evidence->completed_trials) +
            " required=" + std::to_string(policy.min_completed_trials_per_branch));
    if (!enough) {
      fail(DecisionOutcome::INSUFFICIENT_EVIDENCE, FailureKind::INSUFFICIENT_EVIDENCE,
           "the candidate branch holds fewer authoritative completed trials than the policy requires");
    }
  }
  if (baseline_evidence != nullptr) {
    const bool enough = baseline_evidence->completed_trials >= policy.min_completed_trials_per_branch;
    add("baseline_completed_trials", enough,
        "completed=" + std::to_string(baseline_evidence->completed_trials) +
            " required=" + std::to_string(policy.min_completed_trials_per_branch));
    if (!enough) {
      fail(DecisionOutcome::INSUFFICIENT_EVIDENCE, FailureKind::INSUFFICIENT_EVIDENCE,
           "the baseline branch holds fewer authoritative completed trials than the policy requires");
    }
  }

  for (const MetricDefinition& metric : record.definition.metrics) {
    if (!metric.required) {
      continue;
    }
    const MetricAggregate* aggregate = nullptr;
    const bool present = candidate_evidence != nullptr &&
                         branch_has_metric(*candidate_evidence, metric.id, &aggregate) && aggregate->present;
    add("candidate_required_metric:" + metric.name, present,
        present ? "present" : "required metric has no authoritative aggregate on the candidate branch");
    if (!present) {
      fail(DecisionOutcome::INSUFFICIENT_EVIDENCE, FailureKind::INSUFFICIENT_EVIDENCE,
           "a required metric has no authoritative aggregate on the candidate branch");
    } else if (aggregate->valid_count < policy.min_valid_observations_per_required_metric) {
      add("candidate_valid_observations:" + metric.name, false,
          "valid=" + std::to_string(aggregate->valid_count) +
              " required=" + std::to_string(policy.min_valid_observations_per_required_metric));
      fail(DecisionOutcome::INSUFFICIENT_EVIDENCE, FailureKind::INSUFFICIENT_EVIDENCE,
           "a required metric holds fewer valid observations than the policy requires");
    }
    if (baseline_evidence != nullptr) {
      const MetricAggregate* baseline_aggregate = nullptr;
      const bool baseline_present =
          branch_has_metric(*baseline_evidence, metric.id, &baseline_aggregate) && baseline_aggregate->present;
      add("baseline_required_metric:" + metric.name, baseline_present,
          baseline_present ? "present" : "required metric has no authoritative aggregate on the baseline branch");
      if (!baseline_present) {
        fail(DecisionOutcome::INSUFFICIENT_EVIDENCE, FailureKind::INSUFFICIENT_EVIDENCE,
             "a required metric has no authoritative aggregate on the baseline branch");
      } else if (baseline_aggregate->valid_count < policy.min_valid_observations_per_required_metric) {
        add("baseline_valid_observations:" + metric.name, false,
            "valid=" + std::to_string(baseline_aggregate->valid_count) +
                " required=" + std::to_string(policy.min_valid_observations_per_required_metric));
        fail(DecisionOutcome::INSUFFICIENT_EVIDENCE, FailureKind::INSUFFICIENT_EVIDENCE,
             "a required metric holds fewer valid observations than the policy requires on the baseline branch");
      }
    }
    if (policy.reject_on_any_invalid_observation) {
      std::uint32_t invalid = 0;
      if (candidate_evidence != nullptr && branch_has_metric(*candidate_evidence, metric.id, &aggregate) &&
          aggregate != nullptr) {
        invalid += aggregate->invalid_count;
      }
      if (baseline_evidence != nullptr &&
          branch_has_metric(*baseline_evidence, metric.id, &aggregate) && aggregate != nullptr) {
        invalid += aggregate->invalid_count;
      }
      add("no_invalid_observations:" + metric.name, invalid == 0,
          "invalid=" + std::to_string(invalid));
      if (invalid != 0) {
        fail(DecisionOutcome::INVALID, FailureKind::INVALID_OBSERVATION,
             "the policy rejects any decision supported by unresolved invalid observations");
      }
    }
  }

  if (policy.require_environment_match) {
    std::set<std::string> fingerprints;
    std::uint32_t missing = 0;
    for (const TrialRecord& trial : [&record, &candidate, baseline]() {
           std::vector<TrialRecord> trials;
           for (const auto& entry : record.trials) {
             if (entry.second.state != TrialState::COMPLETED) {
               continue;
             }
             if (entry.second.branch == candidate.id || (baseline != nullptr && entry.second.branch == baseline->id)) {
               trials.push_back(entry.second);
             }
           }
           return trials;
         }()) {
      if (trial.environment_fingerprint.empty()) {
        ++missing;
      } else {
        fingerprints.insert(trial.environment_fingerprint);
      }
    }
    const bool matched = fingerprints.size() <= 1;
    add("environment_match", matched,
        matched ? (fingerprints.empty() ? "no environment fingerprint declared" : *fingerprints.begin())
                : "compared branches were produced under different environment fingerprints");
    if (!matched) {
      fail(DecisionOutcome::INVALID, FailureKind::POLICY_REJECTION,
           "the compared branches were produced under different environment fingerprints");
    }
    if (missing != 0 && !matched) {
      reason.append("; ");
      reason.append(std::to_string(missing));
      reason.append(" compared trials declared no environment fingerprint");
    }
  }
}

Decision Coordinator::Impl::build_decision(const ExperimentRecord& record, BranchId candidate_id, DecisionId id,
                                           std::uint64_t sequence) const {
  Decision decision;
  decision.id = id;
  decision.experiment = record.definition.id;
  decision.generation = record.definition.generation;
  decision.hypothesis = record.definition.hypothesis;
  decision.hypothesis_revision = record.definition.hypothesis_revision;
  decision.policy = record.definition.policy.id;
  decision.policy_generation = record.definition.policy.generation;
  decision.candidate = candidate_id;
  decision.sequence = sequence;
  decision.rejected_evidence = record.rejected_evidence;

  const BranchDefinition* candidate = nullptr;
  const BranchDefinition* baseline = nullptr;
  std::vector<BranchId> compared;
  for (const BranchDefinition& branch : record.definition.branches) {
    compared.push_back(branch.id);
    if (branch.id == candidate_id) {
      candidate = &branch;
    }
    if (branch.role == BranchRole::BASELINE && baseline == nullptr) {
      baseline = &branch;
    }
  }
  std::sort(compared.begin(), compared.end());
  decision.compared_branches = compared;

  const std::vector<BranchEvidence> evidence = compute_evidence(record);
  decision.evidence = evidence;

  if (candidate == nullptr) {
    decision.outcome = DecisionOutcome::INVALID;
    decision.reason_kind = FailureKind::POLICY_REJECTION;
    decision.reason = "the candidate branch does not exist in this experiment generation";
    decision.constraints.push_back(ConstraintResult{"candidate_exists", false, decision.reason});
    decision.canonical_state_digest = canonical::evidence_digest(evidence, record.definition);
    return decision;
  }
  if (baseline != nullptr) {
    decision.baseline = baseline->id;
    decision.baseline_present = true;
    if (baseline->id == candidate->id) {
      decision.outcome = DecisionOutcome::INVALID;
      decision.reason_kind = FailureKind::POLICY_REJECTION;
      decision.reason = "the candidate branch is the designated baseline";
      decision.constraints.push_back(ConstraintResult{"candidate_is_not_baseline", false, decision.reason});
      decision.canonical_state_digest = canonical::evidence_digest(evidence, record.definition);
      return decision;
    }
  }

  apply_constraints(record, *candidate, baseline, evidence, decision.constraints, decision.outcome,
                    decision.reason_kind, decision.reason);

  const BranchEvidence* candidate_evidence = nullptr;
  const BranchEvidence* baseline_evidence = nullptr;
  for (const BranchEvidence& entry : evidence) {
    if (entry.branch == candidate->id) {
      candidate_evidence = &entry;
    }
    if (baseline != nullptr && entry.branch == baseline->id) {
      baseline_evidence = &entry;
    }
  }

  std::vector<ComparisonRule> rules = record.definition.policy.rules;
  std::stable_sort(rules.begin(), rules.end(), [](const ComparisonRule& lhs, const ComparisonRule& rhs) {
    if (lhs.priority != rhs.priority) {
      return lhs.priority < rhs.priority;
    }
    return lhs.metric < rhs.metric;
  });

  bool any_decisive = false;
  bool any_decisive_failed = false;
  bool any_nonzero_delta = false;
  std::string first_failed_metric;
  for (const ComparisonRule& rule : rules) {
    if (rule.op == ComparisonOp::REPORT_ONLY) {
      continue;
    }
    any_decisive = true;
    const MetricDefinition* metric = nullptr;
    for (const MetricDefinition& definition : record.definition.metrics) {
      if (definition.id == rule.metric) {
        metric = &definition;
      }
    }
    ComparisonFactor factor;
    factor.metric = rule.metric;
    factor.op = rule.op;
    factor.priority = rule.priority;
    factor.threshold = rule.threshold;
    const MetricAggregate* candidate_aggregate = nullptr;
    const MetricAggregate* baseline_aggregate = nullptr;
    const bool have_candidate =
        candidate_evidence != nullptr && branch_has_metric(*candidate_evidence, rule.metric, &candidate_aggregate) &&
        candidate_aggregate != nullptr && candidate_aggregate->present;
    const bool have_baseline =
        baseline_evidence != nullptr && branch_has_metric(*baseline_evidence, rule.metric, &baseline_aggregate) &&
        baseline_aggregate != nullptr && baseline_aggregate->present;
    factor.has_values = have_candidate && have_baseline;
    const std::string name = metric_name(record, rule.metric);
    if (!factor.has_values) {
      factor.satisfied = false;
      factor.decisive = true;
      factor.detail = "comparison factor " + name + " could not be evaluated: authoritative aggregate missing";
      any_decisive_failed = true;
      if (first_failed_metric.empty()) {
        first_failed_metric = name;
      }
      decision.factors.push_back(std::move(factor));
      continue;
    }
    factor.baseline_value = baseline_aggregate->value;
    factor.candidate_value = candidate_aggregate->value;
    factor.delta = factor.candidate_value - factor.baseline_value;
    if (factor.delta != 0.0) {
      any_nonzero_delta = true;
    }
    const MetricDirection direction = metric == nullptr ? MetricDirection::MINIMIZE : metric->direction;
    switch (rule.op) {
      case ComparisonOp::GREATER_THAN:
        factor.satisfied = factor.candidate_value > factor.baseline_value + rule.threshold;
        break;
      case ComparisonOp::LESS_THAN:
        factor.satisfied = factor.candidate_value < factor.baseline_value - rule.threshold;
        break;
      case ComparisonOp::MIN_IMPROVEMENT:
        factor.satisfied = improvement(direction, factor.baseline_value, factor.candidate_value) >= rule.threshold;
        break;
      case ComparisonOp::MAX_REGRESSION_TOLERANCE:
        factor.satisfied = (-improvement(direction, factor.baseline_value, factor.candidate_value)) <= rule.threshold;
        break;
      case ComparisonOp::TARGET_RANGE:
        factor.satisfied = factor.candidate_value >= rule.target_low && factor.candidate_value <= rule.target_high;
        break;
      case ComparisonOp::MAJORITY_WINS: {
        const std::size_t pairs = std::min(candidate_aggregate->raw_values.size(),
                                           baseline_aggregate->raw_values.size());
        if (pairs == 0) {
          factor.satisfied = false;
        } else {
          std::size_t wins = 0;
          for (std::size_t index = 0; index < pairs; ++index) {
            const double candidate_value = candidate_aggregate->raw_values[index];
            const double baseline_value = baseline_aggregate->raw_values[index];
            if (improvement(direction, baseline_value, candidate_value) > 0.0) {
              ++wins;
            }
          }
          const double fraction = static_cast<double>(wins) / static_cast<double>(pairs);
          factor.satisfied = fraction >= rule.threshold;
          factor.delta = fraction;
        }
        break;
      }
      case ComparisonOp::REPORT_ONLY:
        factor.satisfied = true;
        break;
    }
    factor.decisive = true;
    factor.detail = "metric " + name + " baseline=" + std::to_string(factor.baseline_value) +
                    " candidate=" + std::to_string(factor.candidate_value) +
                    " threshold=" + std::to_string(rule.threshold);
    if (!factor.satisfied) {
      any_decisive_failed = true;
      if (first_failed_metric.empty()) {
        first_failed_metric = name;
      }
    }
    decision.factors.push_back(std::move(factor));
  }

  if (record.definition.policy.use_weighted_score) {
    double score = 0.0;
    bool evaluated = false;
    for (const MetricDefinition& metric : record.definition.metrics) {
      if (!metric.required || metric.weight == 0 || metric.direction == MetricDirection::INFORMATIONAL) {
        continue;
      }
      const MetricAggregate* candidate_aggregate = nullptr;
      const MetricAggregate* baseline_aggregate = nullptr;
      if (candidate_evidence == nullptr || baseline_evidence == nullptr ||
          !branch_has_metric(*candidate_evidence, metric.id, &candidate_aggregate) ||
          !branch_has_metric(*baseline_evidence, metric.id, &baseline_aggregate) ||
          candidate_aggregate == nullptr || baseline_aggregate == nullptr || !candidate_aggregate->present ||
          !baseline_aggregate->present) {
        continue;
      }
      const double reference = std::fabs(baseline_aggregate->value) > 1e-12 ? std::fabs(baseline_aggregate->value) : 1.0;
      score += static_cast<double>(metric.weight) *
               (improvement(metric.direction, baseline_aggregate->value, candidate_aggregate->value) / reference);
      evaluated = true;
    }
    if (evaluated) {
      ComparisonFactor factor;
      factor.metric = MetricId{};
      factor.op = ComparisonOp::REPORT_ONLY;
      factor.satisfied = score >= record.definition.policy.accept_score_threshold;
      factor.decisive = true;
      factor.delta = score;
      factor.has_values = true;
      factor.detail = "explicit weighted score=" + std::to_string(score) +
                      " threshold=" + std::to_string(record.definition.policy.accept_score_threshold);
      if (!factor.satisfied) {
        any_decisive_failed = true;
        if (first_failed_metric.empty()) {
          first_failed_metric = "weighted-score";
        }
      }
      decision.factors.push_back(std::move(factor));
    }
  }

  if (decision.outcome == DecisionOutcome::ACCEPT) {
    if (!any_decisive) {
      decision.outcome = DecisionOutcome::INCONCLUSIVE;
      decision.reason_kind = FailureKind::INSUFFICIENT_EVIDENCE;
      decision.reason = "the policy declares no decisive comparison factor; the result is reported but not decided";
    } else if (any_decisive_failed) {
      decision.outcome = DecisionOutcome::REJECT;
      decision.reason_kind = FailureKind::POLICY_REJECTION;
      decision.reason = "comparison factor failed for metric " + first_failed_metric;
    } else if (!any_nonzero_delta) {
      // Every decisive metric is exactly equal. A tie is not an acceptance.
      decision.outcome = DecisionOutcome::INCONCLUSIVE;
      decision.reason_kind = FailureKind::INSUFFICIENT_EVIDENCE;
      decision.reason = "every decisive comparison factor is exactly equal; a tie is not an acceptance";
      decision.tied = true;
      decision.tie_break_rule = "exact-equality-on-all-decisive-metrics";
    }
  }

  decision.canonical_state_digest = canonical::evidence_digest(evidence, record.definition);
  return decision;
}

Result<Decision> Coordinator::evaluate(ExperimentId experiment, BranchId candidate) const {
  Impl& impl = *impl_;
  std::shared_lock<std::shared_mutex> lock(impl.mutex);
  const ExperimentRecord* record = impl.find_experiment(experiment);
  if (record == nullptr) {
    return make_error(ErrorCode::NOT_FOUND, ErrorStage::DECISION, "experiment does not exist");
  }
  return impl.build_decision(*record, candidate, DecisionId{}, impl.current_sequence());
}

Result<Decision> Coordinator::finalize_experiment(ExperimentId experiment, BranchId candidate, std::string reason) {
  Impl& impl = *impl_;
  Decision decision;
  {
    std::unique_lock<std::shared_mutex> lock(impl.mutex);
    ExperimentRecord* record = impl.find_experiment(experiment);
    if (record == nullptr) {
      return make_error(ErrorCode::NOT_FOUND, ErrorStage::DECISION, "experiment does not exist");
    }
    if (record->state == ExperimentState::FINALIZED) {
      return lifecycle_error(ErrorCode::INVALID_TRANSITION, "the experiment already holds an authoritative decision");
    }
    if (record->decisions.size() >= impl.limits.max_decisions_per_experiment) {
      return make_error(ErrorCode::RESOURCE_EXHAUSTED, ErrorStage::LIMITS, "too many decisions");
    }
    const std::uint64_t sequence = impl.next_sequence();
    Decision built = impl.build_decision(*record, candidate, impl.alloc_decision_id(), sequence);
    if (!reason.empty()) {
      built.reason.append("; caller reason: ");
      built.reason.append(reason);
    }
    record->decisions.push_back(built);
    decision = built;

    switch (built.outcome) {
      case DecisionOutcome::ACCEPT:
      case DecisionOutcome::REJECT:
      case DecisionOutcome::INVALID:
        record->state = ExperimentState::FINALIZED;
        break;
      case DecisionOutcome::CANCELLED:
        record->state = ExperimentState::CANCELLED;
        break;
      case DecisionOutcome::SUPERSEDED:
        record->state = ExperimentState::SUPERSEDED;
        break;
      case DecisionOutcome::INCONCLUSIVE:
      case DecisionOutcome::INSUFFICIENT_EVIDENCE:
        // The experiment stays open: an insufficient result is never forced
        // into a binary outcome.
        break;
    }

    if (built.outcome == DecisionOutcome::ACCEPT || built.outcome == DecisionOutcome::REJECT ||
        built.outcome == DecisionOutcome::INVALID) {
      RollbackPoint point;
      point.id = impl.alloc_rollback_id();
      point.experiment = experiment;
      point.generation = record->definition.generation;
      point.decision = built.id;
      point.decision_present = true;
      point.state_digest = canonical::definition_digest(record->definition);
      point.sequence = sequence;
      for (const BranchDefinition& branch : record->definition.branches) {
        BranchAuthoritySnapshot snapshot;
        snapshot.branch = branch.id;
        snapshot.generation = branch.generation;
        snapshot.state = branch.state;
        point.authority.push_back(snapshot);
      }
      record->rollback_points.push_back(std::move(point));
    }
    record->revision_sequence = sequence;
    impl.log(*record, std::string("experiment-finalized outcome=") + std::string(to_string(built.outcome)) +
                          " decision=" + built.id.to_string());
    const Status persisted = impl.persist_locked();
    if (!persisted.ok()) {
      return persisted;
    }
  }
  impl.notify_decision(decision);
  return decision;
}

Status Coordinator::rollback(ExperimentId experiment, ExperimentGeneration target, std::string reason) {
  Impl& impl = *impl_;
  {
    std::unique_lock<std::shared_mutex> lock(impl.mutex);
    ExperimentRecord* record = impl.find_experiment(experiment);
    if (record == nullptr) {
      return make_error(ErrorCode::NOT_FOUND, ErrorStage::LIFECYCLE, "experiment does not exist");
    }
    if (record->state == ExperimentState::CANCELLED) {
      return lifecycle_error(ErrorCode::INVALID_TRANSITION, "a cancelled experiment cannot be rolled back");
    }
    if (!target.valid()) {
      return make_error(ErrorCode::INVALID_ARGUMENT, ErrorStage::LIFECYCLE,
                        "rollback target generation must be non-null");
    }
    const RollbackPoint* point = nullptr;
    for (const RollbackPoint& candidate : record->rollback_points) {
      if (candidate.generation == target) {
        point = &candidate;
      }
    }
    if (point == nullptr) {
      return make_error(ErrorCode::NOT_FOUND, ErrorStage::LIFECYCLE,
                        "no authoritative rollback point exists for the requested generation");
    }
    const RollbackPoint chosen = *point;
    const ExperimentGeneration previous_generation = record->definition.generation;
    if (chosen.generation == previous_generation && record->state == ExperimentState::OPEN) {
      return lifecycle_error(ErrorCode::INVALID_TRANSITION,
                             "the experiment is already at the requested authoritative generation");
    }

    const std::uint64_t sequence = impl.next_sequence();
    // Every live attempt is abandoned and every non-terminal trial is
    // superseded: post-target work must not survive the rollback as authority.
    impl.abandon_stale_live_work(*record, chosen.generation, sequence);

    record->definition.generation = chosen.generation;
    std::set<BranchId> restored;
    for (const BranchAuthoritySnapshot& snapshot : chosen.authority) {
      BranchDefinition* branch = impl.find_branch(*record, snapshot.branch);
      if (branch == nullptr) {
        continue;
      }
      branch->state = snapshot.state;
      branch->generation = snapshot.generation;
      restored.insert(snapshot.branch);
    }
    for (BranchDefinition& branch : record->definition.branches) {
      if (restored.find(branch.id) != restored.end()) {
        continue;
      }
      // A branch created after the rollback target did not exist at that point.
      if (branch.state == BranchState::ACTIVE) {
        branch.state = BranchState::RETIRED;
      } else if (branch.state != BranchState::SUPERSEDED) {
        branch.state = BranchState::SUPERSEDED;
      }
    }

    RollbackPoint recorded;
    recorded.id = impl.alloc_rollback_id();
    recorded.experiment = experiment;
    recorded.generation = chosen.generation;
    recorded.restored_from = previous_generation;
    recorded.state_digest = canonical::definition_digest(record->definition);
    recorded.authority = chosen.authority;
    recorded.sequence = sequence;
    record->rollback_points.push_back(std::move(recorded));

    if (record->state == ExperimentState::FINALIZED || record->state == ExperimentState::SUPERSEDED) {
      record->state = ExperimentState::OPEN;
    }
    record->revision_sequence = sequence;
    impl.log(*record, "rollback restored-generation=" + chosen.generation.to_string() +
                          " from-generation=" + previous_generation.to_string() + " reason=" + reason);

    const Status validated = validate_experiment_record(*record, impl.limits, experiment);
    if (!validated.ok()) {
      return validated;
    }
    const Status persisted = impl.persist_locked();
    if (!persisted.ok()) {
      return persisted;
    }
  }
  impl.notify_event("rollback:" + experiment.to_string());
  return Status::success();
}

}  // namespace experiment_fabric
