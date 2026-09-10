// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "experiment_fabric/inspect.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <string>

#include "experiment_fabric/canonical.hpp"
#include "experiment_fabric/version.hpp"

namespace experiment_fabric {
namespace inspect {
namespace {

void push_limited(std::vector<std::string>& lines, const Limits& limits, std::string line) {
  if (lines.size() >= limits.max_explanation_lines) {
    return;
  }
  lines.push_back(std::move(line));
}

std::string id_text(const auto& id) { return id.to_string(); }

std::string describe_metric(const ExperimentDefinition& definition, MetricId id) {
  for (const MetricDefinition& metric : definition.metrics) {
    if (metric.id == id) {
      return metric.name;
    }
  }
  return std::string("metric#") + id.to_string();
}

const BranchDefinition* branch_of(const ExperimentDefinition& definition, BranchId id) {
  for (const BranchDefinition& branch : definition.branches) {
    if (branch.id == id) {
      return &branch;
    }
  }
  return nullptr;
}

void render_branch_tree(const ExperimentDefinition& definition, BranchId node, std::uint32_t depth,
                        std::vector<std::string>& lines, const Limits& limits) {
  if (depth > limits.max_lineage_depth) {
    return;
  }
  const BranchDefinition* branch = branch_of(definition, node);
  if (branch == nullptr) {
    return;
  }
  std::string line(depth * 2, ' ');
  line.append("branch ");
  line.append(branch->id.to_string());
  line.append(" name=");
  line.append(branch->name);
  line.append(" role=");
  line.append(to_string(branch->role));
  line.append(" state=");
  line.append(to_string(branch->state));
  line.append(" branch-generation=");
  line.append(branch->generation.to_string());
  if (branch->parent.valid()) {
    line.append(" parent=");
    line.append(branch->parent.to_string());
  }
  push_limited(lines, limits, std::move(line));
  std::vector<BranchId> children;
  for (const BranchDefinition& candidate : definition.branches) {
    if (candidate.parent == node) {
      children.push_back(candidate.id);
    }
  }
  std::sort(children.begin(), children.end());
  for (const BranchId& child : children) {
    render_branch_tree(definition, child, depth + 1, lines, limits);
  }
}

}  // namespace

std::string format_real(double value) {
  if (std::isnan(value)) {
    return "nan";
  }
  if (std::isinf(value)) {
    return value > 0 ? "inf" : "-inf";
  }
  char buffer[48];
  const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::general);
  return std::string(buffer, result.ptr);
}

std::string digest_hex(const Sha256::Digest& digest) { return to_hex(digest); }

std::string_view provenance_label(Provenance provenance) noexcept { return to_string(provenance); }

std::vector<std::string> render_snapshot(const ExperimentSnapshot& snapshot, const Limits& limits) {
  std::vector<std::string> lines;
  push_limited(lines, limits, std::string("experiment-fabric ") + std::string(version_string()));
  push_limited(lines, limits, "experiment " + id_text(snapshot.definition.id) + " name=" + snapshot.definition.name);
  push_limited(lines, limits, "state " + std::string(to_string(snapshot.state)));
  push_limited(lines, limits, "generation " + snapshot.definition.generation.to_string());
  push_limited(lines, limits, "hypothesis " + snapshot.hypothesis.id.to_string() + " revision " +
                                  snapshot.hypothesis.revision.to_string());
  push_limited(lines, limits, "policy " + snapshot.definition.policy.id.to_string() + " generation " +
                                  snapshot.definition.policy.generation.to_string());
  push_limited(lines, limits, "producing-epoch " + snapshot.producing_epoch.to_string());
  push_limited(lines, limits, "state-digest " + digest_hex(snapshot.state_digest));
  push_limited(lines, limits, "trials " + std::to_string(snapshot.trials.size()));
  push_limited(lines, limits, "observations " + std::to_string(snapshot.observations.size()));
  push_limited(lines, limits, "artifacts " + std::to_string(snapshot.artifacts.size()));
  push_limited(lines, limits, "decisions " + std::to_string(snapshot.decisions.size()));
  push_limited(lines, limits, "rollback-points " + std::to_string(snapshot.rollback_points.size()));
  push_limited(lines, limits, "rejected-evidence " + std::to_string(snapshot.rejected_evidence.size()));
  const std::vector<std::string> branches = render_branches(snapshot, limits);
  lines.insert(lines.end(), branches.begin(), branches.end());
  const std::vector<std::string> facts = render_hypothesis(snapshot, limits);
  lines.insert(lines.end(), facts.begin(), facts.end());
  for (const std::string& entry : snapshot.lifecycle_log) {
    push_limited(lines, limits, "log " + entry);
  }
  return lines;
}

std::vector<std::string> render_hypothesis(const ExperimentSnapshot& snapshot, const Limits& limits) {
  std::vector<std::string> lines;
  push_limited(lines, limits, "hypothesis " + snapshot.hypothesis.id.to_string() + " revision " +
                                  snapshot.hypothesis.revision.to_string() + " statement=" +
                                  snapshot.hypothesis.statement);
  if (snapshot.hypothesis.expected_direction.has_value()) {
    push_limited(lines, limits, "expected-direction " +
                                    std::string(to_string(*snapshot.hypothesis.expected_direction)));
  }
  if (snapshot.hypothesis.expected_metric.valid()) {
    push_limited(lines, limits, "expected-metric " +
                                    describe_metric(snapshot.definition, snapshot.hypothesis.expected_metric));
  }
  if (!snapshot.hypothesis.provenance.empty()) {
    push_limited(lines, limits, "provenance " + snapshot.hypothesis.provenance);
  }
  push_limited(lines, limits, "revision-history " + std::to_string(snapshot.hypothesis_history.size()));
  for (const Hypothesis& entry : snapshot.hypothesis_history) {
    push_limited(lines, limits,
                 "  revision " + entry.revision.to_string() + " superseded=" +
                     (entry.superseded ? "true" : "false") + " superseded-by=" +
                     entry.superseded_by_revision.to_string() + " statement=" + entry.statement);
  }
  return lines;
}

std::vector<std::string> render_branches(const ExperimentSnapshot& snapshot, const Limits& limits) {
  std::vector<std::string> lines;
  std::vector<BranchDefinition> branches = snapshot.definition.branches;
  std::sort(branches.begin(), branches.end(),
            [](const BranchDefinition& lhs, const BranchDefinition& rhs) { return lhs.id < rhs.id; });
  for (const BranchDefinition& branch : branches) {
    std::string line = "branch ";
    line.append(branch.id.to_string());
    line.append(" name=");
    line.append(branch.name);
    line.append(" role=");
    line.append(to_string(branch.role));
    line.append(" state=");
    line.append(to_string(branch.state));
    line.append(" branch-generation=");
    line.append(branch.generation.to_string());
    line.append(" planned-trials=");
    line.append(std::to_string(branch.planned_trials));
    line.append(" forked=");
    line.append(branch.forked ? "true" : "false");
    if (branch.parent.valid()) {
      line.append(" parent=");
      line.append(branch.parent.to_string());
    }
    push_limited(lines, limits, std::move(line));
    for (const ParameterAssignment& parameter : branch.parameters) {
      push_limited(lines, limits, "  parameter " + parameter.key + "[" + std::string(to_string(parameter.kind)) +
                                      "]=" + parameter.value);
    }
    if (branch.input_set_present) {
      push_limited(lines, limits, "  input-set " + branch.input_set.to_string());
    }
    if (branch.environment_present) {
      push_limited(lines, limits, "  environment " + branch.environment.to_string());
    }
    if (!branch.environment_fingerprint.empty()) {
      push_limited(lines, limits, "  environment-fingerprint " + branch.environment_fingerprint);
    }
    if (branch.seed.has_value()) {
      push_limited(lines, limits, "  seed " + std::to_string(*branch.seed));
    }
  }
  return lines;
}

std::vector<std::string> render_lineage(const ExperimentSnapshot& snapshot, const Limits& limits) {
  std::vector<std::string> lines;
  push_limited(lines, limits, "lineage experiment " + snapshot.definition.id.to_string() + " hypothesis " +
                                  snapshot.definition.hypothesis.to_string() + " revision " +
                                  snapshot.definition.hypothesis_revision.to_string());
  std::vector<BranchId> roots;
  for (const BranchDefinition& branch : snapshot.definition.branches) {
    if (!branch.parent.valid()) {
      roots.push_back(branch.id);
    }
  }
  std::sort(roots.begin(), roots.end());
  for (const BranchId& root : roots) {
    render_branch_tree(snapshot.definition, root, 0, lines, limits);
  }
  // Any branch whose parent is absent is still reported so that lineage never
  // silently loses a node.
  for (const BranchDefinition& branch : snapshot.definition.branches) {
    if (branch.parent.valid() && branch_of(snapshot.definition, branch.parent) == nullptr) {
      render_branch_tree(snapshot.definition, branch.id, 0, lines, limits);
    }
  }
  return lines;
}

std::vector<std::string> render_trials(const ExperimentSnapshot& snapshot, const Limits& limits) {
  std::vector<std::string> lines;
  for (const TrialSnapshot& entry : snapshot.trials) {
    const TrialRecord& trial = entry.trial;
    std::string line = "trial ";
    line.append(trial.id.to_string());
    line.append(" branch=");
    line.append(trial.branch.to_string());
    line.append(" state=");
    line.append(to_string(trial.state));
    line.append(" generation=");
    line.append(trial.generation.to_string());
    line.append(" branch-generation=");
    line.append(trial.branch_generation.to_string());
    line.append(" attempts=");
    line.append(std::to_string(trial.attempt_count));
    line.append(" committed-attempts=");
    line.append(std::to_string(trial.committed_attempts));
    if (trial.authoritative_attempt.valid()) {
      line.append(" authoritative-attempt=");
      line.append(trial.authoritative_attempt.to_string());
    }
    if (trial.failure_kind != FailureKind::NONE) {
      line.append(" failure=");
      line.append(to_string(trial.failure_kind));
    }
    push_limited(lines, limits, std::move(line));
    if (!trial.failure_detail.empty()) {
      push_limited(lines, limits, "  failure-detail " + trial.failure_detail);
    }
    if (trial.seed.has_value()) {
      push_limited(lines, limits, "  seed " + std::to_string(*trial.seed));
    }
    for (const AttemptRecord& attempt : entry.attempts) {
      std::string attempt_line = "  attempt ";
      attempt_line.append(attempt.id.to_string());
      attempt_line.append(" number=");
      attempt_line.append(attempt.number.to_string());
      attempt_line.append(" state=");
      attempt_line.append(to_string(attempt.state));
      attempt_line.append(" worker=");
      attempt_line.append(attempt.worker.to_string());
      attempt_line.append(" boot=");
      attempt_line.append(attempt.boot.to_string());
      attempt_line.append(" epoch=");
      attempt_line.append(attempt.epoch.to_string());
      attempt_line.append(" committed=");
      attempt_line.append(attempt.committed ? "true" : "false");
      if (attempt.failure_kind != FailureKind::NONE) {
        attempt_line.append(" failure=");
        attempt_line.append(to_string(attempt.failure_kind));
      }
      push_limited(lines, limits, std::move(attempt_line));
    }
  }
  return lines;
}

std::vector<std::string> render_observations(const ExperimentSnapshot& snapshot, const Limits& limits) {
  std::vector<std::string> lines;
  std::vector<Observation> observations = snapshot.observations;
  std::sort(observations.begin(), observations.end(), [](const Observation& lhs, const Observation& rhs) {
    if (lhs.trial != rhs.trial) {
      return lhs.trial < rhs.trial;
    }
    if (lhs.metric != rhs.metric) {
      return lhs.metric < rhs.metric;
    }
    return lhs.id < rhs.id;
  });
  for (const Observation& observation : observations) {
    std::string line = "observation ";
    line.append(observation.id.to_string());
    line.append(" trial=");
    line.append(observation.trial.to_string());
    line.append(" branch=");
    line.append(observation.branch.to_string());
    line.append(" metric=");
    line.append(describe_metric(snapshot.definition, observation.metric));
    line.append(" validity=");
    line.append(to_string(observation.validity));
    line.append(" kind=");
    line.append(to_string(observation.value.kind));
    line.append(" value=");
    switch (observation.value.kind) {
      case MetricKind::REAL:
        line.append(format_real(observation.value.real));
        break;
      case MetricKind::INTEGER:
        line.append(std::to_string(observation.value.integer));
        break;
      case MetricKind::BOOLEAN:
        line.append(observation.value.boolean ? "true" : "false");
        break;
      case MetricKind::CATEGORICAL:
        line.append(observation.value.category);
        break;
    }
    line.append(" attempt=");
    line.append(observation.attempt.to_string());
    line.append(" producer=");
    line.append(observation.producer.to_string());
    push_limited(lines, limits, std::move(line));
    if (!observation.detail.empty()) {
      push_limited(lines, limits, "  detail " + observation.detail);
    }
  }
  return lines;
}

std::vector<std::string> render_rejected_evidence(const ExperimentSnapshot& snapshot, const Limits& limits) {
  std::vector<std::string> lines;
  for (const RejectedEvidence& evidence : snapshot.rejected_evidence) {
    std::string line = "rejected ";
    line.append(evidence.kind);
    line.append(" id=");
    line.append(std::to_string(evidence.identifier));
    line.append(" reason=");
    line.append(to_string(evidence.reason));
    line.append(" detail=");
    line.append(evidence.detail);
    push_limited(lines, limits, std::move(line));
  }
  for (const Observation& observation : snapshot.observations) {
    std::string state;
    switch (observation.validity) {
      case ObservationValidity::VALID:
        continue;
      case ObservationValidity::INVALID:
        state = "INVALID";
        break;
      case ObservationValidity::MISSING:
        state = "MISSING";
        break;
      case ObservationValidity::UNSUPPORTED:
        state = "UNSUPPORTED";
        break;
    }
    std::string line = "non-authoritative observation ";
    line.append(observation.id.to_string());
    line.append(" trial=");
    line.append(observation.trial.to_string());
    line.append(" metric=");
    line.append(describe_metric(snapshot.definition, observation.metric));
    line.append(" validity=");
    line.append(state);
    push_limited(lines, limits, std::move(line));
  }
  for (const TrialRecord& trial : [&snapshot]() {
         std::vector<TrialRecord> trials;
         for (const TrialSnapshot& entry : snapshot.trials) {
           trials.push_back(entry.trial);
         }
         return trials;
       }()) {
    if (trial.state == TrialState::SUPERSEDED || trial.state == TrialState::CANCELLED) {
      push_limited(lines, limits, "trial " + trial.id.to_string() + " state=" + std::string(to_string(trial.state)) +
                                      " detail=" + trial.failure_detail);
    }
  }
  return lines;
}

std::vector<std::string> render_artifacts(const ExperimentSnapshot& snapshot, const Limits& limits) {
  std::vector<std::string> lines;
  std::vector<ArtifactRecord> artifacts = snapshot.artifacts;
  std::sort(artifacts.begin(), artifacts.end(),
            [](const ArtifactRecord& lhs, const ArtifactRecord& rhs) { return lhs.id < rhs.id; });
  for (const ArtifactRecord& artifact : artifacts) {
    std::string line = "artifact ";
    line.append(artifact.id.to_string());
    line.append(" trial=");
    line.append(artifact.trial.to_string());
    line.append(" attempt=");
    line.append(artifact.attempt.to_string());
    line.append(" category=");
    line.append(artifact.category);
    line.append(" current=");
    line.append(artifact.current ? "true" : "false");
    line.append(" production-trusted=false");
    line.append(" locator=");
    line.append(artifact.locator);
    push_limited(lines, limits, std::move(line));
    if (artifact.digest_present) {
      push_limited(lines, limits, "  digest " + digest_hex(artifact.content_digest));
    }
    if (artifact.size_present) {
      push_limited(lines, limits, "  size " + std::to_string(artifact.size_bytes));
    }
  }
  return lines;
}

std::vector<std::string> render_evidence(const BranchEvidence& evidence, const Limits& limits) {
  std::vector<std::string> lines;
  std::string line = "evidence branch ";
  line.append(evidence.branch.to_string());
  line.append(" role=");
  line.append(to_string(evidence.role));
  line.append(" state=");
  line.append(to_string(evidence.state));
  line.append(" completed=");
  line.append(std::to_string(evidence.completed_trials));
  line.append(" failed=");
  line.append(std::to_string(evidence.failed_trials));
  line.append(" cancelled=");
  line.append(std::to_string(evidence.cancelled_trials));
  line.append(" invalid=");
  line.append(std::to_string(evidence.invalid_trials));
  line.append(" superseded=");
  line.append(std::to_string(evidence.superseded_trials));
  line.append(" open=");
  line.append(std::to_string(evidence.open_trials));
  push_limited(lines, limits, std::move(line));
  for (const MetricAggregate& aggregate : evidence.aggregates) {
    std::string metric_line = "  aggregate metric=";
    metric_line.append(aggregate.metric.to_string());
    metric_line.append(" rule=");
    metric_line.append(to_string(aggregate.rule));
    metric_line.append(" present=");
    metric_line.append(aggregate.present ? "true" : "false");
    metric_line.append(" value=");
    metric_line.append(aggregate.present ? format_real(aggregate.value) : std::string("absent"));
    if (!aggregate.category.empty()) {
      metric_line.append(" category=");
      metric_line.append(aggregate.category);
    }
    metric_line.append(" valid=");
    metric_line.append(std::to_string(aggregate.valid_count));
    metric_line.append(" invalid=");
    metric_line.append(std::to_string(aggregate.invalid_count));
    metric_line.append(" missing=");
    metric_line.append(std::to_string(aggregate.missing_count));
    metric_line.append(" unsupported=");
    metric_line.append(std::to_string(aggregate.unsupported_count));
    push_limited(lines, limits, std::move(metric_line));
  }
  return lines;
}

std::vector<std::string> render_decision(const Decision& decision, const Limits& limits) {
  std::vector<std::string> lines;
  push_limited(lines, limits, "decision " + decision.id.to_string() + " experiment " +
                                  decision.experiment.to_string() + " generation " +
                                  decision.generation.to_string());
  push_limited(lines, limits, "hypothesis " + decision.hypothesis.to_string() + " revision " +
                                  decision.hypothesis_revision.to_string());
  push_limited(lines, limits, "policy " + decision.policy.to_string() + " generation " +
                                  decision.policy_generation.to_string());
  std::string compared = "compared-branches";
  for (const BranchId& branch : decision.compared_branches) {
    compared.push_back(' ');
    compared.append(branch.to_string());
  }
  push_limited(lines, limits, std::move(compared));
  push_limited(lines, limits, "candidate " + decision.candidate.to_string() + " baseline " +
                                  (decision.baseline_present ? decision.baseline.to_string() : std::string("none")));
  push_limited(lines, limits, "outcome " + std::string(to_string(decision.outcome)));
  push_limited(lines, limits, "reason-kind " + std::string(to_string(decision.reason_kind)));
  push_limited(lines, limits, "reason " + decision.reason);
  for (const BranchEvidence& evidence : decision.evidence) {
    const std::vector<std::string> evidence_lines = render_evidence(evidence, limits);
    lines.insert(lines.end(), evidence_lines.begin(), evidence_lines.end());
  }
  for (const ConstraintResult& constraint : decision.constraints) {
    push_limited(lines, limits, std::string("constraint ") + (constraint.passed ? "PASS " : "FAIL ") +
                                    constraint.name + " " + constraint.detail);
  }
  for (const ComparisonFactor& factor : decision.factors) {
    std::string line = "factor metric=";
    line.append(factor.metric.valid() ? factor.metric.to_string() : std::string("weighted-score"));
    line.append(" op=");
    line.append(to_string(factor.op));
    line.append(" priority=");
    line.append(std::to_string(factor.priority));
    line.append(" has-values=");
    line.append(factor.has_values ? "true" : "false");
    line.append(" baseline=");
    line.append(format_real(factor.baseline_value));
    line.append(" candidate=");
    line.append(format_real(factor.candidate_value));
    line.append(" delta=");
    line.append(format_real(factor.delta));
    line.append(" satisfied=");
    line.append(factor.satisfied ? "true" : "false");
    push_limited(lines, limits, std::move(line));
  }
  if (decision.tied) {
    push_limited(lines, limits, "tie-break " + decision.tie_break_rule);
  }
  for (const RejectedEvidence& evidence : decision.rejected_evidence) {
    push_limited(lines, limits, "rejected " + evidence.kind + " id=" + std::to_string(evidence.identifier) +
                                    " reason=" + std::string(to_string(evidence.reason)) + " detail=" +
                                    evidence.detail);
  }
  push_limited(lines, limits, "canonical-state-digest " + digest_hex(decision.canonical_state_digest));
  push_limited(lines, limits, "sequence " + std::to_string(decision.sequence));
  return lines;
}

std::vector<std::string> render_reproducibility(const ReproducibilityRecord& record, const Limits& limits) {
  std::vector<std::string> lines;
  push_limited(lines, limits, "reproducibility experiment " + record.experiment.to_string() + " generation " +
                                  record.generation.to_string());
  push_limited(lines, limits, "branch " + record.branch.to_string() + " branch-generation " +
                                  record.branch_generation.to_string());
  push_limited(lines, limits, "hypothesis " + record.hypothesis.to_string() + " revision " +
                                  record.hypothesis_revision.to_string());
  push_limited(lines, limits, "definition-digest " + digest_hex(record.definition_digest));
  push_limited(lines, limits, "branch-definition-digest " + digest_hex(record.branch_definition_digest));
  push_limited(lines, limits, "policy-digest " + digest_hex(record.policy_digest));
  push_limited(lines, limits, "status " + std::string(to_string(record.status)));
  push_limited(lines, limits, std::string("seed ") +
                                  (record.seed.has_value() ? std::to_string(*record.seed) : std::string("absent")));
  push_limited(lines, limits, std::string("input-set ") +
                                  (record.input_set_present ? record.input_set.to_string() : std::string("absent")));
  push_limited(lines, limits,
               std::string("environment ") +
                   (record.environment_present ? record.environment.to_string() : std::string("absent")));
  if (!record.environment_fingerprint.empty()) {
    push_limited(lines, limits, "environment-fingerprint " + record.environment_fingerprint);
  }
  if (!record.executable_identity.empty()) {
    push_limited(lines, limits, "executable-identity " + record.executable_identity);
  }
  if (!record.producer_identity.empty()) {
    push_limited(lines, limits, "producer-identity " + record.producer_identity);
  }
  push_limited(lines, limits, "valid-observations " + std::to_string(record.observation_count));
  push_limited(lines, limits, "artifacts " + std::to_string(record.artifacts.size()));
  for (const ArtifactId& artifact : record.artifacts) {
    push_limited(lines, limits, "  artifact " + artifact.to_string());
  }
  for (const ParameterAssignment& parameter : record.parameters) {
    push_limited(lines, limits, "parameter " + parameter.key + "=" + parameter.value);
  }
  for (const std::string& missing : record.missing_materials) {
    push_limited(lines, limits, "missing-material " + missing);
  }
  return lines;
}

std::string summarize(const ExperimentSnapshot& snapshot) {
  std::string line = "experiment ";
  line.append(snapshot.definition.id.to_string());
  line.append(" name=");
  line.append(snapshot.definition.name);
  line.append(" state=");
  line.append(to_string(snapshot.state));
  line.append(" generation=");
  line.append(snapshot.definition.generation.to_string());
  line.append(" branches=");
  line.append(std::to_string(snapshot.definition.branches.size()));
  line.append(" trials=");
  line.append(std::to_string(snapshot.trials.size()));
  return line;
}

}  // namespace inspect
}  // namespace experiment_fabric
