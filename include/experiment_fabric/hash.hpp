// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef EXPERIMENT_FABRIC_HASH_HPP
#define EXPERIMENT_FABRIC_HASH_HPP

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "experiment_fabric/export.hpp"

namespace experiment_fabric {

/// \file
/// Content digests and integrity checks. Implemented in-tree so that the
/// runtime stays dependency light and so that digest bytes are reproducible
/// on every platform.

/// Incremental SHA-256.
class EF_API Sha256 {
 public:
  static constexpr std::size_t kDigestBytes = 32;
  using Digest = std::array<std::uint8_t, kDigestBytes>;

  Sha256() noexcept { reset(); }

  void reset() noexcept;
  void update(std::span<const std::uint8_t> data) noexcept;
  void update(std::string_view text) noexcept;
  void update_byte(std::uint8_t value) noexcept;
  void update_u8(std::uint8_t value) noexcept { update_byte(value); }
  void update_u16(std::uint16_t value) noexcept;
  void update_u32(std::uint32_t value) noexcept;
  void update_u64(std::uint64_t value) noexcept;
  void update_bool(bool value) noexcept;
  void update_length_prefixed(std::string_view text) noexcept;

  /// Finalises the digest. The object must be reset before reuse.
  [[nodiscard]] Digest finalize() noexcept;

  [[nodiscard]] static Digest digest_of(std::span<const std::uint8_t> data) noexcept;
  [[nodiscard]] static Digest digest_of(std::string_view text) noexcept;

 private:
  std::array<std::uint32_t, 8> state_{};
  std::array<std::uint8_t, 64> buffer_{};
  std::size_t buffer_length_ = 0;
  std::uint64_t total_bytes_ = 0;
};

/// Lowercase hexadecimal rendering of a digest (64 characters).
[[nodiscard]] EF_API std::string to_hex(const Sha256::Digest& digest);

/// Strict lowercase/uppercase hexadecimal parsing into a SHA-256 digest.
[[nodiscard]] EF_API bool parse_digest(std::string_view hex, Sha256::Digest& out) noexcept;

/// CRC-32/ISO-HDLC used for frame and record integrity checks.
[[nodiscard]] EF_API std::uint32_t crc32(std::span<const std::uint8_t> data) noexcept;

/// Incremental CRC-32.
class EF_API Crc32 {
 public:
  void reset() noexcept { state_ = 0xFFFFFFFFu; }
  void update(std::span<const std::uint8_t> data) noexcept;
  [[nodiscard]] std::uint32_t finalize() const noexcept { return state_ ^ 0xFFFFFFFFu; }

 private:
  std::uint32_t state_ = 0xFFFFFFFFu;
};

/// Computes a stable 64-bit FNV-1a value. Used only for non-authoritative
/// bucketing and diagnostics; never for integrity or authority decisions.
[[nodiscard]] EF_API std::uint64_t fnv1a64(std::string_view text) noexcept;

/// Deterministic pseudo random engine used for seeded policies and tests.
///
/// The generator is fully specified by its seed so that a stored seed
/// reproduces the same stream on every platform.
class EF_API DeterministicRng {
 public:
  explicit DeterministicRng(std::uint64_t seed) noexcept : state_(seed ^ 0x9E3779B97F4A7C15ull) {}

  [[nodiscard]] std::uint64_t next_u64() noexcept;
  [[nodiscard]] std::uint32_t next_u32() noexcept;
  [[nodiscard]] std::uint64_t next_bounded(std::uint64_t bound) noexcept;

 private:
  std::uint64_t state_;
};

}  // namespace experiment_fabric

#endif  // EXPERIMENT_FABRIC_HASH_HPP
