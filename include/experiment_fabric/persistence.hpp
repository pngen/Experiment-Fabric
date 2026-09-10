// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef EXPERIMENT_FABRIC_PERSISTENCE_HPP
#define EXPERIMENT_FABRIC_PERSISTENCE_HPP

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "experiment_fabric/error.hpp"
#include "experiment_fabric/limits.hpp"
#include "experiment_fabric/state.hpp"

namespace experiment_fabric {

/// \file
/// Transactional, versioned, integrity-checked persistence.
///
/// Commit protocol: build the complete new image, serialise it to a temporary
/// file in the same directory, flush and close it, then atomically replace the
/// live file. A half-written image therefore never becomes authoritative.
///
/// The format is little-endian, length-prefixed, CRC-32 checked per record, and
/// carries a whole-image SHA-256. Decoding is bounded at every step and rejects
/// truncation, checksum mismatch, unknown versions, impossible enum values,
/// absurd counts, integer overflow, duplicate identities, invalid cross
/// references and trailing garbage.

namespace persistence {

/// Magic bytes: ASCII "EFSTATE1" with a version word appended by the encoder.
inline constexpr std::uint32_t kMagic = 0x54415453u;  // "STAT" little-endian

/// Serialises a complete state image.
[[nodiscard]] Result<std::vector<std::uint8_t>> serialize(const CoordinatorState& state, const Limits& limits);

/// Deserialises and fully validates a state image.
[[nodiscard]] Result<CoordinatorState> deserialize(std::span<const std::uint8_t> bytes, const Limits& limits);

/// Writes a state image transactionally. On success the live file contains
/// exactly the new image; on failure the previous live file is unchanged.
[[nodiscard]] Status save(const std::filesystem::path& path, const CoordinatorState& state, const Limits& limits);

/// Reads and validates a state image. A missing file yields NOT_FOUND.
[[nodiscard]] Result<CoordinatorState> load(const std::filesystem::path& path, const Limits& limits);

/// Removes any temporary images left by an interrupted commit. Returns the
/// number of files removed.
[[nodiscard]] Result<std::uint32_t> discard_temporary_images(const std::filesystem::path& path);

}  // namespace persistence
}  // namespace experiment_fabric

#endif  // EXPERIMENT_FABRIC_PERSISTENCE_HPP
