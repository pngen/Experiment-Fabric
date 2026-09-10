// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "experiment_fabric/model.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>

namespace experiment_fabric {
namespace {

std::string format_integer(std::int64_t value) {
  char buffer[24];
  const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
  return std::string(buffer, result.ptr);
}

}  // namespace

ParameterAssignment make_text_parameter(std::string key, std::string value) {
  ParameterAssignment assignment;
  assignment.key = std::move(key);
  assignment.kind = ParameterKind::TEXT;
  assignment.value = std::move(value);
  return assignment;
}

ParameterAssignment make_integer_parameter(std::string key, std::int64_t value) {
  ParameterAssignment assignment;
  assignment.key = std::move(key);
  assignment.kind = ParameterKind::INTEGER;
  assignment.value = format_integer(value);
  return assignment;
}

ParameterAssignment make_real_parameter(std::string key, double value) {
  ParameterAssignment assignment;
  assignment.key = std::move(key);
  assignment.kind = ParameterKind::REAL;
  char buffer[64];
  const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
  assignment.value.assign(buffer, result.ptr);
  return assignment;
}

ParameterAssignment make_boolean_parameter(std::string key, bool value) {
  ParameterAssignment assignment;
  assignment.key = std::move(key);
  assignment.kind = ParameterKind::BOOLEAN;
  assignment.value = value ? "true" : "false";
  return assignment;
}

bool parameter_less(const ParameterAssignment& lhs, const ParameterAssignment& rhs) noexcept {
  if (lhs.key != rhs.key) {
    return lhs.key < rhs.key;
  }
  if (lhs.kind != rhs.kind) {
    return static_cast<std::uint8_t>(lhs.kind) < static_cast<std::uint8_t>(rhs.kind);
  }
  return lhs.value < rhs.value;
}

bool is_finite_value(const MetricValue& value) noexcept {
  switch (value.kind) {
    case MetricKind::REAL:
      return std::isfinite(value.real);
    case MetricKind::INTEGER:
    case MetricKind::BOOLEAN:
    case MetricKind::CATEGORICAL:
      return true;
  }
  return false;
}

}  // namespace experiment_fabric
