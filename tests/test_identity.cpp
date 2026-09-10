// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <string>
#include <type_traits>

#include "experiment_fabric/identity.hpp"
#include "test_support.hpp"

namespace {
namespace ef = experiment_fabric;
}  // namespace

EF_TEST(identity, null_identity_is_never_valid) {
  const ef::ExperimentId id;
  EF_CHECK(id.is_null());
  EF_CHECK(!id.valid());
  EF_CHECK_EQ(id.to_string(), std::string("0"));
  const ef::TrialAttemptId attempt;
  EF_CHECK(!attempt.valid());
}

EF_TEST(identity, canonical_parse_round_trip) {
  const ef::BranchId id = ef::BranchId::from_value(18446744073709551615ull);
  EF_CHECK_EQ(id.to_string(), std::string("18446744073709551615"));
  const auto parsed = ef::BranchId::parse(id.to_string());
  EF_REQUIRE(parsed.has_value());
  EF_CHECK_EQ(parsed->value(), id.value());
}

EF_TEST(identity, parse_rejects_non_canonical_text) {
  EF_CHECK(!ef::BranchId::parse("").has_value());
  EF_CHECK(!ef::BranchId::parse(" 1").has_value());
  EF_CHECK(!ef::BranchId::parse("1 ").has_value());
  EF_CHECK(!ef::BranchId::parse("+1").has_value());
  EF_CHECK(!ef::BranchId::parse("-1").has_value());
  EF_CHECK(!ef::BranchId::parse("01").has_value());
  EF_CHECK(!ef::BranchId::parse("1a").has_value());
  EF_CHECK(!ef::BranchId::parse("18446744073709551616").has_value());
  EF_CHECK(!ef::BranchId::parse("0x10").has_value());
}

EF_TEST(identity, ordering_is_numeric) {
  const ef::TrialId low = ef::TrialId::from_value(2);
  const ef::TrialId high = ef::TrialId::from_value(10);
  EF_CHECK(low < high);
  EF_CHECK(!(high < low));
  EF_CHECK(low != high);
}

EF_TEST(identity, allocator_never_reuses_and_never_returns_null) {
  ef::IdAllocator<ef::TrialId> allocator;
  ef::TrialId previous;
  for (int index = 0; index < 1024; ++index) {
    const ef::TrialId next = allocator.next();
    EF_CHECK(next.valid());
    EF_CHECK(previous < next);
    previous = next;
  }
  allocator.observe(ef::TrialId::from_value(100000));
  EF_CHECK(allocator.next().value() == 100001);
}

EF_TEST(identity, distinct_identity_kinds_are_distinct_types) {
  const ef::BranchId branch = ef::BranchId::from_value(7);
  const ef::TrialId trial = ef::TrialId::from_value(7);
  EF_CHECK_EQ(branch.value(), trial.value());
  static_assert(!std::is_same_v<ef::BranchId, ef::TrialId>);
  static_assert(!std::is_same_v<ef::ExperimentGeneration, ef::HypothesisRevision>);
  static_assert(!std::is_same_v<ef::WorkerId, ef::WorkerBootId>);
  static_assert(!std::is_same_v<ef::CoordinatorEpoch, ef::WorkerBootId>);
}
