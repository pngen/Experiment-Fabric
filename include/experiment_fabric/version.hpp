// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef EXPERIMENT_FABRIC_VERSION_HPP
#define EXPERIMENT_FABRIC_VERSION_HPP

#include <cstdint>
#include <string_view>

namespace experiment_fabric {

/// Semantic version of the Experiment Fabric runtime.
inline constexpr std::uint32_t kVersionMajor = 1;
inline constexpr std::uint32_t kVersionMinor = 0;
inline constexpr std::uint32_t kVersionPatch = 0;

/// Human readable version string, e.g. "1.0.0".
std::string_view version_string() noexcept;

/// Persistence format version understood by this build.
inline constexpr std::uint32_t kPersistenceFormatVersion = 1;

/// Wire protocol version understood by this build.
inline constexpr std::uint16_t kProtocolVersion = 1;

}  // namespace experiment_fabric

#endif  // EXPERIMENT_FABRIC_VERSION_HPP
