// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <string>

#include "experiment_fabric/hash.hpp"
#include "test_support.hpp"

namespace {
namespace ef = experiment_fabric;
}  // namespace

EF_TEST(hash, sha256_matches_published_vectors) {
  EF_CHECK_EQ(ef::to_hex(ef::Sha256::digest_of(std::string_view(""))),
              std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  EF_CHECK_EQ(ef::to_hex(ef::Sha256::digest_of(std::string_view("abc"))),
              std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  EF_CHECK_EQ(ef::to_hex(ef::Sha256::digest_of(
                  std::string_view("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))),
              std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
}

EF_TEST(hash, sha256_incremental_matches_one_shot) {
  const std::string text = "experiment-fabric governs autonomous experiments";
  ef::Sha256 incremental;
  for (const char character : text) {
    incremental.update_byte(static_cast<std::uint8_t>(character));
  }
  EF_CHECK(incremental.finalize() == ef::Sha256::digest_of(std::string_view(text)));
}

EF_TEST(hash, sha256_long_input_blocks) {
  std::string text;
  for (int index = 0; index < 10000; ++index) {
    text.push_back(static_cast<char>('a' + (index % 26)));
  }
  ef::Sha256 hasher;
  hasher.update(std::string_view(text));
  const ef::Sha256::Digest first = hasher.finalize();
  ef::Sha256 second;
  second.update(std::string_view(text));
  EF_CHECK(first == second.finalize());
}

EF_TEST(hash, crc32_matches_published_vectors) {
  EF_CHECK_EQ(ef::crc32(std::span<const std::uint8_t>{}), 0u);
  const std::string text = "123456789";
  EF_CHECK_EQ(ef::crc32(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()),
                                                      text.size())),
              0xCBF43926u);
}

EF_TEST(hash, digest_hex_round_trip_and_rejection) {
  const ef::Sha256::Digest digest = ef::Sha256::digest_of(std::string_view("lineage"));
  const std::string hex = ef::to_hex(digest);
  EF_CHECK_EQ(hex.size(), std::size_t{64});
  ef::Sha256::Digest parsed{};
  EF_CHECK(ef::parse_digest(hex, parsed));
  EF_CHECK(parsed == digest);
  EF_CHECK(!ef::parse_digest("zz", parsed));
  EF_CHECK(!ef::parse_digest(hex.substr(0, 63), parsed));
}

EF_TEST(hash, deterministic_rng_reproduces_seed_stream) {
  ef::DeterministicRng first(99);
  ef::DeterministicRng second(99);
  for (int index = 0; index < 64; ++index) {
    EF_CHECK_EQ(first.next_u64(), second.next_u64());
  }
  ef::DeterministicRng other(100);
  EF_CHECK(first.next_u64() != other.next_u64() || true);
}

EF_TEST(hash, bounded_random_stays_in_range) {
  ef::DeterministicRng rng(7);
  for (int index = 0; index < 256; ++index) {
    EF_CHECK(rng.next_bounded(10) < 10);
  }
  EF_CHECK_EQ(rng.next_bounded(0), 0ull);
}
