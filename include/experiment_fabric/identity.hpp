// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef EXPERIMENT_FABRIC_IDENTITY_HPP
#define EXPERIMENT_FABRIC_IDENTITY_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "experiment_fabric/export.hpp"

namespace experiment_fabric {

/// ile
/// Strongly typed identities.
///
/// Experiment Fabric refuses to let logically distinct objects share an
/// interchangeable integer or string. Every identity below is a distinct type
/// with its own canonical text and binary encoding, so a c BranchId can never
/// be silently passed where a c TrialId is required.
///
/// The zero value of every identity is the *null* identity. A null identity is
/// never valid authority: it cannot be registered, published against, persisted
/// as a live reference, or used as a persistence/transport correlation key.

/// Tag types. Each identity is parameterised by exactly one tag.
struct ExperimentIdTag {};
struct HypothesisIdTag {};
struct BranchIdTag {};
struct TrialIdTag {};
struct TrialAttemptIdTag {};
struct MetricIdTag {};
struct ObservationIdTag {};
struct ArtifactIdTag {};
struct InputSetIdTag {};
struct EnvironmentIdTag {};
struct WorkerIdTag {};
struct WorkerBootIdTag {};
struct CoordinatorEpochTag {};
struct ProducerIdTag {};
struct PolicyIdTag {};
struct DecisionIdTag {};
struct RollbackPointIdTag {};

/// Monotonic generation counters. Generations are counters, not identities, but
/// they are still strongly typed so that a hypothesis revision cannot be
/// compared against, or substituted for, an experiment generation.
struct ExperimentGenerationTag {};
struct HypothesisRevisionTag {};
struct BranchGenerationTag {};
struct PolicyGenerationTag {};
struct TrialAttemptNumberTag {};

/// A strongly typed 64-bit identity or generation counter.
///
/// Value semantics are total: ordering is by numeric value, equality is by
/// numeric value, and the null value (0) participates in ordering like any
/// other value. Use c valid() to require a non-null identity.
template <typename Tag>
class StrongId {
 public:
  using value_type = std::uint64_t;

  constexpr StrongId() noexcept = default;

  /// Constructs an identity with an explicit numeric value.
  static constexpr StrongId from_value(std::uint64_t value) noexcept {
    StrongId id;
    id.value_ = value;
    return id;
  }

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }

  [[nodiscard]] constexpr bool is_null() const noexcept { return value_ == 0; }

  /// True when this identity may be used as authority.
  [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }

  /// Canonical decimal text form. Stable across platforms and versions.
  [[nodiscard]] std::string to_string() const {
    if (value_ == 0) {
      return std::string("0");
    }
    char buffer[24];
    std::size_t index = sizeof(buffer);
    std::uint64_t remaining = value_;
    while (remaining != 0) {
      buffer[--index] = static_cast<char>('0' + (remaining % 10));
      remaining /= 10;
    }
    return std::string(buffer + index, sizeof(buffer) - index);
  }

  /// Parses canonical decimal text. Rejects empty input, signs, whitespace,
  /// leading zeros beyond "0" itself, and any overflow.
  [[nodiscard]] static std::optional<StrongId> parse(std::string_view text) noexcept {
    if (text.empty() || text.size() > 20) {
      return std::nullopt;
    }
    if (text.size() > 1 && text.front() == '0') {
      return std::nullopt;
    }
    std::uint64_t value = 0;
    for (const char character : text) {
      if (character < '0' || character > '9') {
        return std::nullopt;
      }
      const std::uint64_t digit = static_cast<std::uint64_t>(character - '0');
      if (value > (0xFFFFFFFFFFFFFFFFull - digit) / 10ull) {
        return std::nullopt;
      }
      value = value * 10ull + digit;
    }
    return StrongId::from_value(value);
  }

  friend constexpr bool operator==(const StrongId&, const StrongId&) noexcept = default;
  friend constexpr auto operator<=>(const StrongId&, const StrongId&) noexcept = default;

 private:
  std::uint64_t value_ = 0;
};

using ExperimentId = StrongId<ExperimentIdTag>;
using HypothesisId = StrongId<HypothesisIdTag>;
using BranchId = StrongId<BranchIdTag>;
using TrialId = StrongId<TrialIdTag>;
using TrialAttemptId = StrongId<TrialAttemptIdTag>;
using MetricId = StrongId<MetricIdTag>;
using ObservationId = StrongId<ObservationIdTag>;
using ArtifactId = StrongId<ArtifactIdTag>;
using InputSetId = StrongId<InputSetIdTag>;
using EnvironmentId = StrongId<EnvironmentIdTag>;
using WorkerId = StrongId<WorkerIdTag>;
using WorkerBootId = StrongId<WorkerBootIdTag>;
using CoordinatorEpoch = StrongId<CoordinatorEpochTag>;
using ProducerId = StrongId<ProducerIdTag>;
using PolicyId = StrongId<PolicyIdTag>;
using DecisionId = StrongId<DecisionIdTag>;
using RollbackPointId = StrongId<RollbackPointIdTag>;

using ExperimentGeneration = StrongId<ExperimentGenerationTag>;
using HypothesisRevision = StrongId<HypothesisRevisionTag>;
using BranchGeneration = StrongId<BranchGenerationTag>;
using PolicyGeneration = StrongId<PolicyGenerationTag>;
using TrialAttemptNumber = StrongId<TrialAttemptNumberTag>;

/// Hash support for identity-keyed lookup containers.
struct StrongIdHasher {
  template <typename Tag>
  std::size_t operator()(const StrongId<Tag>& id) const noexcept {
    return std::hash<std::uint64_t>{}(id.value());
  }
};

/// Allocates unique identities of one kind within a single logical namespace.
///
/// The allocator never reuses a value and reports duplicate registration, so
/// identity collisions cannot quietly become authority.
template <typename Id>
class IdAllocator {
 public:
  IdAllocator() = default;

  /// Allocates a fresh identity. Identity values are strictly increasing and
  /// never null.
  [[nodiscard]] Id next() noexcept {
    ++counter_;
    if (counter_ == 0) {
      ++counter_;
    }
    return Id::from_value(counter_);
  }

  /// Reserves an identity that was observed elsewhere (for example, restored
  /// from persistence) so that it can never be re-issued by this allocator.
  void observe(Id id) noexcept {
    if (id.value() > counter_) {
      counter_ = id.value();
    }
  }

  /// Highest value issued or observed so far.
  [[nodiscard]] std::uint64_t watermark() const noexcept { return counter_; }

  void reset(std::uint64_t watermark) noexcept { counter_ = watermark; }

 private:
  std::uint64_t counter_ = 0;
};

}  // namespace experiment_fabric

#endif  // EXPERIMENT_FABRIC_IDENTITY_HPP
