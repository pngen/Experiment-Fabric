// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "experiment_fabric/persistence.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "experiment_fabric/protocol.hpp"
#include "experiment_fabric/version.hpp"

namespace experiment_fabric {
namespace persistence {
namespace {

constexpr std::uint32_t kHeaderBytes = 56;

enum class RecordType : std::uint32_t {
  STATE_HEADER = 1,
  WORKER = 2,
  EXPERIMENT = 3,
};

Status corrupt(std::string reason) {
  return make_error(ErrorCode::CORRUPT_PERSISTENCE, ErrorStage::PERSISTENCE, std::move(reason));
}

Status io_failure(std::string reason) {
  return make_error(ErrorCode::PERSISTENCE_FAILURE, ErrorStage::PERSISTENCE, std::move(reason));
}

void store_u32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
  for (int index = 0; index < 4; ++index) {
    bytes[offset + static_cast<std::size_t>(index)] = static_cast<std::uint8_t>((value >> (8 * index)) & 0xFFu);
  }
}

void store_u64(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint64_t value) {
  for (int index = 0; index < 8; ++index) {
    bytes[offset + static_cast<std::size_t>(index)] = static_cast<std::uint8_t>((value >> (8 * index)) & 0xFFu);
  }
}

std::uint32_t load_u32(std::span<const std::uint8_t> bytes, std::size_t offset) {
  std::uint32_t value = 0;
  for (int index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(bytes[offset + static_cast<std::size_t>(index)]) << (8 * index);
  }
  return value;
}

std::uint64_t load_u64(std::span<const std::uint8_t> bytes, std::size_t offset) {
  std::uint64_t value = 0;
  for (int index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(bytes[offset + static_cast<std::size_t>(index)]) << (8 * index);
  }
  return value;
}

void append_record(std::vector<std::uint8_t>& body, RecordType type, const std::vector<std::uint8_t>& payload) {
  const std::size_t offset = body.size();
  body.resize(offset + 12);
  store_u32(body, offset, static_cast<std::uint32_t>(type));
  store_u32(body, offset + 4, static_cast<std::uint32_t>(payload.size()));
  store_u32(body, offset + 8, crc32(payload));
  body.insert(body.end(), payload.begin(), payload.end());
}

/// Maps a codec failure into the persistence error space while preserving
/// resource-exhaustion semantics.
Status map_codec_status(const Status& status) {
  if (status.code() == ErrorCode::RESOURCE_EXHAUSTED) {
    return status;
  }
  std::string reason = "record decode failed: ";
  reason.append(status.to_string());
  return corrupt(std::move(reason));
}

}  // namespace

Result<std::vector<std::uint8_t>> serialize(const CoordinatorState& state, const Limits& limits) {
  if (state.format_version != kPersistenceFormatVersion) {
    return io_failure("refusing to serialize state with a mismatched format version");
  }
  if (state.total_record_count() > limits.max_persisted_records) {
    return make_error(ErrorCode::RESOURCE_EXHAUSTED, ErrorStage::LIMITS,
                      "state holds more records than the configured bound");
  }

  std::vector<std::uint8_t> body;
  {
    PayloadWriter writer(limits);
    writer.u32(state.format_version);
    writer.u64(state.last_epoch);
    writer.u64(state.last_sequence);
    writer.u64(state.next_worker_registration_sequence);
    writer.u64(state.id_watermark_experiment);
    writer.u64(state.id_watermark_hypothesis);
    writer.u64(state.id_watermark_branch);
    writer.u64(state.id_watermark_metric);
    writer.u64(state.id_watermark_policy);
    writer.u64(state.id_watermark_trial);
    writer.u64(state.id_watermark_attempt);
    writer.u64(state.id_watermark_observation);
    writer.u64(state.id_watermark_artifact);
    writer.u64(state.id_watermark_decision);
    writer.u64(state.id_watermark_rollback);
    append_record(body, RecordType::STATE_HEADER, std::move(writer).take());
  }
  for (const WorkerIncarnation& incarnation : state.workers) {
    PayloadWriter writer(limits);
    write_worker_incarnation(writer, incarnation);
    append_record(body, RecordType::WORKER, std::move(writer).take());
  }
  for (const auto& entry : state.experiments) {
    PayloadWriter writer(limits);
    writer.u64(entry.first.value());
    write_experiment_record(writer, entry.second);
    append_record(body, RecordType::EXPERIMENT, std::move(writer).take());
  }

  const Sha256::Digest body_digest = Sha256::digest_of(body);

  std::vector<std::uint8_t> image(kHeaderBytes + body.size(), 0);
  store_u32(image, 0, kMagic);
  store_u32(image, 4, state.format_version);
  store_u32(image, 8, 0);
  store_u32(image, 12, crc32(std::span<const std::uint8_t>(image.data(), 12)));
  store_u64(image, 16, static_cast<std::uint64_t>(body.size()));
  std::copy(body_digest.begin(), body_digest.end(), image.begin() + 24);
  std::copy(body.begin(), body.end(), image.begin() + static_cast<std::ptrdiff_t>(kHeaderBytes));
  return image;
}

Result<CoordinatorState> deserialize(std::span<const std::uint8_t> bytes, const Limits& limits) {
  if (bytes.size() < kHeaderBytes) {
    return corrupt("state image is shorter than its header");
  }
  if (bytes.size() > limits.max_state_file_bytes) {
    return make_error(ErrorCode::RESOURCE_EXHAUSTED, ErrorStage::LIMITS, "state image exceeds the configured bound");
  }
  if (load_u32(bytes, 0) != kMagic) {
    return corrupt("state image magic mismatch");
  }
  const std::uint32_t format_version = load_u32(bytes, 4);
  if (format_version != kPersistenceFormatVersion) {
    return corrupt("state image format version is not supported by this build");
  }
  if (load_u32(bytes, 8) != 0) {
    return corrupt("state image flags must be zero");
  }
  if (load_u32(bytes, 12) != crc32(bytes.first(12))) {
    return corrupt("state image header checksum mismatch");
  }
  const std::uint64_t body_length = load_u64(bytes, 16);
  if (body_length != bytes.size() - kHeaderBytes) {
    if (body_length > bytes.size() - kHeaderBytes) {
      return corrupt("state image is truncated");
    }
    return corrupt("state image has trailing garbage");
  }
  const std::span<const std::uint8_t> body = bytes.subspan(kHeaderBytes);
  Sha256 hasher;
  hasher.update(body);
  const Sha256::Digest computed = hasher.finalize();
  Sha256::Digest stored{};
  std::copy(bytes.begin() + 24, bytes.begin() + 24 + static_cast<std::ptrdiff_t>(stored.size()), stored.begin());
  if (computed != stored) {
    return corrupt("state image body digest mismatch");
  }

  CoordinatorState state;
  bool header_seen = false;
  std::size_t offset = 0;
  std::uint64_t record_count = 0;
  while (offset < body.size()) {
    if (body.size() - offset < 12) {
      return corrupt("record header is truncated");
    }
    const std::uint32_t raw_type = load_u32(body, offset);
    const std::uint32_t payload_length = load_u32(body, offset + 4);
    const std::uint32_t payload_crc = load_u32(body, offset + 8);
    if (raw_type < static_cast<std::uint32_t>(RecordType::STATE_HEADER) ||
        raw_type > static_cast<std::uint32_t>(RecordType::EXPERIMENT)) {
      return corrupt("record type is not recognised");
    }
    if (payload_length > body.size() - offset - 12) {
      return corrupt("record payload is truncated");
    }
    if (++record_count > limits.max_persisted_records) {
      return make_error(ErrorCode::RESOURCE_EXHAUSTED, ErrorStage::LIMITS, "state holds too many records");
    }
    const std::span<const std::uint8_t> payload = body.subspan(offset + 12, payload_length);
    if (crc32(payload) != payload_crc) {
      return corrupt("record checksum mismatch");
    }

    PayloadReader reader(payload, limits);
    switch (static_cast<RecordType>(raw_type)) {
      case RecordType::STATE_HEADER: {
        if (header_seen) {
          return corrupt("state image contains more than one header record");
        }
        header_seen = true;
        auto version = reader.u32();
        if (!version.ok()) {
          return map_codec_status(version.status());
        }
        state.format_version = version.value();
        auto last_epoch = reader.u64();
        if (!last_epoch.ok()) {
          return map_codec_status(last_epoch.status());
        }
        state.last_epoch = last_epoch.value();
        auto last_sequence = reader.u64();
        if (!last_sequence.ok()) {
          return map_codec_status(last_sequence.status());
        }
        state.last_sequence = last_sequence.value();
        auto registration = reader.u64();
        if (!registration.ok()) {
          return map_codec_status(registration.status());
        }
        state.next_worker_registration_sequence = registration.value();
        for (std::uint64_t* target : {&state.id_watermark_experiment, &state.id_watermark_hypothesis,
                                      &state.id_watermark_branch, &state.id_watermark_metric,
                                      &state.id_watermark_policy, &state.id_watermark_trial,
                                      &state.id_watermark_attempt, &state.id_watermark_observation,
                                      &state.id_watermark_artifact, &state.id_watermark_decision,
                                      &state.id_watermark_rollback}) {
          auto value = reader.u64();
          if (!value.ok()) {
            return map_codec_status(value.status());
          }
          *target = value.value();
        }
        break;
      }
      case RecordType::WORKER: {
        auto incarnation = read_worker_incarnation(reader);
        if (!incarnation.ok()) {
          return map_codec_status(incarnation.status());
        }
        if (state.workers.size() >= limits.max_registered_workers) {
          return make_error(ErrorCode::RESOURCE_EXHAUSTED, ErrorStage::LIMITS,
                            "state holds more worker incarnations than the configured bound");
        }
        state.workers.push_back(incarnation.value());
        break;
      }
      case RecordType::EXPERIMENT: {
        auto id = reader.u64();
        if (!id.ok()) {
          return map_codec_status(id.status());
        }
        const ExperimentId experiment = ExperimentId::from_value(id.value());
        if (!experiment.valid()) {
          return corrupt("experiment record carries a null identity");
        }
        auto record = read_experiment_record(reader);
        if (!record.ok()) {
          return map_codec_status(record.status());
        }
        if (state.experiments.size() >= limits.max_experiments) {
          return make_error(ErrorCode::RESOURCE_EXHAUSTED, ErrorStage::LIMITS,
                            "state holds more experiments than the configured bound");
        }
        if (!state.experiments.emplace(experiment, std::move(record.value())).second) {
          return corrupt("duplicate experiment record");
        }
        break;
      }
    }
    const Status end = reader.expect_end();
    if (!end.ok()) {
      return corrupt("record contains trailing bytes");
    }
    offset += 12 + payload_length;
  }
  if (!header_seen) {
    return corrupt("state image is missing its header record");
  }
  const Status validated = state.validate(limits);
  if (!validated.ok()) {
    return validated;
  }
  return state;
}

Status save(const std::filesystem::path& path, const CoordinatorState& state, const Limits& limits) {
  auto image = serialize(state, limits);
  if (!image.ok()) {
    return image.status();
  }
  std::error_code error;
  const std::filesystem::path parent = path.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent, error);
    if (error && !std::filesystem::exists(parent)) {
      return io_failure("cannot create the state directory");
    }
  }
  static std::atomic<std::uint64_t> sequence{0};
  const std::string temporary_name =
      path.filename().string() + ".tmp." + std::to_string(sequence.fetch_add(1) + 1);
  const std::filesystem::path temporary = parent.empty() ? std::filesystem::path(temporary_name)
                                                         : parent / temporary_name;

  {
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream.is_open()) {
      return io_failure("cannot open the temporary state image for writing");
    }
    stream.write(reinterpret_cast<const char*>(image.value().data()),
                 static_cast<std::streamsize>(image.value().size()));
    stream.flush();
    if (!stream.good()) {
      stream.close();
      std::filesystem::remove(temporary, error);
      return io_failure("failed while writing the temporary state image");
    }
    stream.close();
    if (stream.fail()) {
      std::filesystem::remove(temporary, error);
      return io_failure("failed while closing the temporary state image");
    }
  }

  std::filesystem::rename(temporary, path, error);
  if (error) {
    std::error_code cleanup_error;
    std::filesystem::remove(temporary, cleanup_error);
    return io_failure("atomic replacement of the state image failed");
  }
  return Status::success();
}

Result<CoordinatorState> load(const std::filesystem::path& path, const Limits& limits) {
  std::error_code error;
  if (!std::filesystem::exists(path, error)) {
    return make_error(ErrorCode::NOT_FOUND, ErrorStage::PERSISTENCE, "no persisted state image exists");
  }
  const std::uintmax_t size = std::filesystem::file_size(path, error);
  if (error) {
    return io_failure("cannot determine the size of the state image");
  }
  if (size > limits.max_state_file_bytes) {
    return make_error(ErrorCode::RESOURCE_EXHAUSTED, ErrorStage::LIMITS, "state image exceeds the configured bound");
  }
  std::ifstream stream(path, std::ios::binary);
  if (!stream.is_open()) {
    return io_failure("cannot open the state image for reading");
  }
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  if (size != 0) {
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    if (stream.gcount() != static_cast<std::streamsize>(size)) {
      return corrupt("state image could not be read in full");
    }
  }
  return deserialize(bytes, limits);
}

Result<std::uint32_t> discard_temporary_images(const std::filesystem::path& path) {
  std::error_code error;
  const std::filesystem::path parent = path.parent_path();
  const std::filesystem::path directory = parent.empty() ? std::filesystem::path(".") : parent;
  if (!std::filesystem::exists(directory, error)) {
    return std::uint32_t{0};
  }
  const std::string prefix = path.filename().string() + ".tmp.";
  std::uint32_t removed = 0;
  for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(directory, error)) {
    if (error) {
      return io_failure("cannot enumerate the state directory");
    }
    const std::string name = entry.path().filename().string();
    if (name.rfind(prefix, 0) == 0) {
      std::error_code remove_error;
      if (std::filesystem::remove(entry.path(), remove_error)) {
        ++removed;
      }
    }
  }
  return removed;
}

}  // namespace persistence
}  // namespace experiment_fabric
