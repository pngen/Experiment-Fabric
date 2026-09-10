// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "experiment_fabric/hash.hpp"

#include <array>
#include <cstring>

namespace experiment_fabric {
namespace {

constexpr std::array<std::uint32_t, 64> kSha256Constants = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

constexpr std::uint32_t rotr32(std::uint32_t value, std::uint32_t bits) noexcept {
  return (value >> bits) | (value << (32u - bits));
}

constexpr std::array<std::uint32_t, 256> make_crc_table() noexcept {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t index = 0; index < 256; ++index) {
    std::uint32_t value = index;
    for (int bit = 0; bit < 8; ++bit) {
      value = (value & 1u) != 0u ? (0xEDB88320u ^ (value >> 1)) : (value >> 1);
    }
    table[index] = value;
  }
  return table;
}

constexpr std::array<std::uint32_t, 256> kCrcTable = make_crc_table();

}  // namespace

void Sha256::reset() noexcept {
  state_[0] = 0x6a09e667u;
  state_[1] = 0xbb67ae85u;
  state_[2] = 0x3c6ef372u;
  state_[3] = 0xa54ff53au;
  state_[4] = 0x510e527fu;
  state_[5] = 0x9b05688cu;
  state_[6] = 0x1f83d9abu;
  state_[7] = 0x5be0cd19u;
  buffer_length_ = 0;
  total_bytes_ = 0;
}

void Sha256::update_byte(std::uint8_t value) noexcept {
  buffer_[buffer_length_++] = value;
  ++total_bytes_;
  if (buffer_length_ == buffer_.size()) {
    std::uint32_t words[64];
    for (std::size_t index = 0; index < 16; ++index) {
      const std::size_t base = index * 4;
      words[index] = (static_cast<std::uint32_t>(buffer_[base]) << 24) |
                     (static_cast<std::uint32_t>(buffer_[base + 1]) << 16) |
                     (static_cast<std::uint32_t>(buffer_[base + 2]) << 8) |
                     static_cast<std::uint32_t>(buffer_[base + 3]);
    }
    for (std::size_t index = 16; index < 64; ++index) {
      const std::uint32_t s0 = rotr32(words[index - 15], 7) ^ rotr32(words[index - 15], 18) ^ (words[index - 15] >> 3);
      const std::uint32_t s1 = rotr32(words[index - 2], 17) ^ rotr32(words[index - 2], 19) ^ (words[index - 2] >> 10);
      words[index] = words[index - 16] + s0 + words[index - 7] + s1;
    }
    std::uint32_t a = state_[0];
    std::uint32_t b = state_[1];
    std::uint32_t c = state_[2];
    std::uint32_t d = state_[3];
    std::uint32_t e = state_[4];
    std::uint32_t f = state_[5];
    std::uint32_t g = state_[6];
    std::uint32_t h = state_[7];
    for (std::size_t index = 0; index < 64; ++index) {
      const std::uint32_t big_s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
      const std::uint32_t choose = (e & f) ^ ((~e) & g);
      const std::uint32_t temp1 = h + big_s1 + choose + kSha256Constants[index] + words[index];
      const std::uint32_t big_s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
      const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
      const std::uint32_t temp2 = big_s0 + majority;
      h = g;
      g = f;
      f = e;
      e = d + temp1;
      d = c;
      c = b;
      b = a;
      a = temp1 + temp2;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
    buffer_length_ = 0;
  }
}

void Sha256::update(std::span<const std::uint8_t> data) noexcept {
  for (const std::uint8_t byte : data) {
    update_byte(byte);
  }
}

void Sha256::update(std::string_view text) noexcept {
  for (const char character : text) {
    update_byte(static_cast<std::uint8_t>(character));
  }
}

void Sha256::update_u16(std::uint16_t value) noexcept {
  update_byte(static_cast<std::uint8_t>(value & 0xFFu));
  update_byte(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
}

void Sha256::update_u32(std::uint32_t value) noexcept {
  for (int shift = 0; shift < 32; shift += 8) {
    update_byte(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
  }
}

void Sha256::update_u64(std::uint64_t value) noexcept {
  for (int shift = 0; shift < 64; shift += 8) {
    update_byte(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
  }
}

void Sha256::update_bool(bool value) noexcept { update_byte(value ? 1u : 0u); }

void Sha256::update_length_prefixed(std::string_view text) noexcept {
  update_u64(static_cast<std::uint64_t>(text.size()));
  update(text);
}

Sha256::Digest Sha256::finalize() noexcept {
  const std::uint64_t bit_length = total_bytes_ * 8ull;
  update_byte(0x80u);
  while (buffer_length_ != 56) {
    update_byte(0x00u);
  }
  // The length is appended big-endian and must not disturb the byte counter.
  for (int shift = 56; shift >= 0; shift -= 8) {
    buffer_[buffer_length_++] = static_cast<std::uint8_t>((bit_length >> shift) & 0xFFu);
  }
  std::uint32_t words[64];
  for (std::size_t index = 0; index < 16; ++index) {
    const std::size_t base = index * 4;
    words[index] = (static_cast<std::uint32_t>(buffer_[base]) << 24) |
                   (static_cast<std::uint32_t>(buffer_[base + 1]) << 16) |
                   (static_cast<std::uint32_t>(buffer_[base + 2]) << 8) |
                   static_cast<std::uint32_t>(buffer_[base + 3]);
  }
  for (std::size_t index = 16; index < 64; ++index) {
    const std::uint32_t s0 = rotr32(words[index - 15], 7) ^ rotr32(words[index - 15], 18) ^ (words[index - 15] >> 3);
    const std::uint32_t s1 = rotr32(words[index - 2], 17) ^ rotr32(words[index - 2], 19) ^ (words[index - 2] >> 10);
    words[index] = words[index - 16] + s0 + words[index - 7] + s1;
  }
  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];
  for (std::size_t index = 0; index < 64; ++index) {
    const std::uint32_t big_s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
    const std::uint32_t choose = (e & f) ^ ((~e) & g);
    const std::uint32_t temp1 = h + big_s1 + choose + kSha256Constants[index] + words[index];
    const std::uint32_t big_s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
    const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = big_s0 + majority;
    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }
  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;

  Digest digest{};
  for (std::size_t index = 0; index < 8; ++index) {
    digest[index * 4] = static_cast<std::uint8_t>((state_[index] >> 24) & 0xFFu);
    digest[index * 4 + 1] = static_cast<std::uint8_t>((state_[index] >> 16) & 0xFFu);
    digest[index * 4 + 2] = static_cast<std::uint8_t>((state_[index] >> 8) & 0xFFu);
    digest[index * 4 + 3] = static_cast<std::uint8_t>(state_[index] & 0xFFu);
  }
  reset();
  return digest;
}

Sha256::Digest Sha256::digest_of(std::span<const std::uint8_t> data) noexcept {
  Sha256 hasher;
  hasher.update(data);
  return hasher.finalize();
}

Sha256::Digest Sha256::digest_of(std::string_view text) noexcept {
  Sha256 hasher;
  hasher.update(text);
  return hasher.finalize();
}

std::string to_hex(const Sha256::Digest& digest) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string result;
  result.reserve(digest.size() * 2);
  for (const std::uint8_t byte : digest) {
    result.push_back(kHex[(byte >> 4) & 0x0Fu]);
    result.push_back(kHex[byte & 0x0Fu]);
  }
  return result;
}

bool parse_digest(std::string_view hex, Sha256::Digest& out) noexcept {
  if (hex.size() != out.size() * 2) {
    return false;
  }
  auto nibble = [](char character) -> int {
    if (character >= '0' && character <= '9') {
      return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
      return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
      return character - 'A' + 10;
    }
    return -1;
  };
  for (std::size_t index = 0; index < out.size(); ++index) {
    const int high = nibble(hex[index * 2]);
    const int low = nibble(hex[index * 2 + 1]);
    if (high < 0 || low < 0) {
      return false;
    }
    out[index] = static_cast<std::uint8_t>((high << 4) | low);
  }
  return true;
}

void Crc32::update(std::span<const std::uint8_t> data) noexcept {
  std::uint32_t state = state_;
  for (const std::uint8_t byte : data) {
    state = kCrcTable[(state ^ byte) & 0xFFu] ^ (state >> 8);
  }
  state_ = state;
}

std::uint32_t crc32(std::span<const std::uint8_t> data) noexcept {
  Crc32 checksum;
  checksum.update(data);
  return checksum.finalize();
}

std::uint64_t fnv1a64(std::string_view text) noexcept {
  std::uint64_t value = 0xCBF29CE484222325ull;
  for (const char character : text) {
    value ^= static_cast<std::uint8_t>(character);
    value *= 0x100000001B3ull;
  }
  return value;
}

std::uint64_t DeterministicRng::next_u64() noexcept {
  state_ += 0x9E3779B97F4A7C15ull;
  std::uint64_t value = state_;
  value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
  value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
  return value ^ (value >> 31);
}

std::uint32_t DeterministicRng::next_u32() noexcept {
  return static_cast<std::uint32_t>(next_u64() >> 32);
}

std::uint64_t DeterministicRng::next_bounded(std::uint64_t bound) noexcept {
  if (bound == 0) {
    return 0;
  }
  return next_u64() % bound;
}

}  // namespace experiment_fabric
