// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "experiment_fabric/client.hpp"
#include "experiment_fabric/inspect.hpp"
#include "experiment_fabric/version.hpp"

namespace {

namespace ef = experiment_fabric;

void print_usage() {
  std::cout
      << "ef_ctl " << ef::version_string() << " — deterministic inspection and control client\n"
      << "usage: ef_ctl --host <address> --port <n> <command> [arguments]\n"
      << "commands:\n"
      << "  status                                  coordinator status lines\n"
      << "  list                                    list experiments\n"
      << "  workers                                 list live worker incarnations\n"
      << "  validate                                validate the durable state image\n"
      << "  snapshot <experiment>                   full deterministic snapshot\n"
      << "  hypothesis <experiment>                 hypothesis and revision history\n"
      << "  branches <experiment>                   branch definitions\n"
      << "  lineage <experiment>                    branch lineage tree\n"
      << "  trials <experiment>                     trials and attempt history\n"
      << "  observations <experiment>               metric observations\n"
      << "  rejected <experiment>                   stale, invalid and rejected evidence\n"
      << "  artifacts <experiment>                  artifact references\n"
      << "  repro <experiment>:<branch>             reproducibility record\n"
      << "  explain <experiment>:<branch>           comparison explanation\n"
      << "  finalize <experiment> <branch> [reason] commit an authoritative decision\n"
      << "  create-trial <experiment> <branch>      create one logical trial\n"
      << "  fork <experiment> <parent> <name>       fork a branch\n"
      << "  cancel-trial <trial> [reason]           cancel a logical trial\n"
      << "  cancel-experiment <experiment> [reason] cancel an experiment\n"
      << "  rollback <experiment> <generation>      roll back to an authoritative generation\n"
      << "  replay <frames-file>                    replay preserved frames and report each reply\n"
      << "  shutdown                                request an orderly coordinator stop\n";
}

bool take_value(int argc, char** argv, int& index, std::string& out) {
  if (index + 1 >= argc) {
    return false;
  }
  out = argv[++index];
  return true;
}

int print_lines(const ef::Result<std::vector<std::string>>& lines) {
  if (!lines.ok()) {
    std::cerr << "query failed: " << lines.status().to_string() << "\n";
    return 5;
  }
  for (const std::string& line : lines.value()) {
    std::cout << line << "\n";
  }
  return 0;
}

bool parse_pair(const std::string& text, ef::ExperimentId& experiment, ef::BranchId& branch) {
  const std::size_t separator = text.find(':');
  if (separator == std::string::npos) {
    return false;
  }
  const auto parsed_experiment = ef::ExperimentId::parse(text.substr(0, separator));
  const auto parsed_branch = ef::BranchId::parse(text.substr(separator + 1));
  if (!parsed_experiment.has_value() || !parsed_branch.has_value()) {
    return false;
  }
  experiment = *parsed_experiment;
  branch = *parsed_branch;
  return true;
}

int replay_frames(ef::Client& client, const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream.is_open()) {
    std::cerr << "cannot open frame log: " << path << "\n";
    return 4;
  }
  std::uint32_t index = 0;
  std::uint32_t rejected = 0;
  while (true) {
    std::uint64_t length = 0;
    stream.read(reinterpret_cast<char*>(&length), sizeof(length));
    if (stream.gcount() != static_cast<std::streamsize>(sizeof(length))) {
      break;
    }
    if (length == 0 || length > ef::Limits::defaults().max_frame_payload_bytes + ef::kFrameHeaderBytes) {
      std::cerr << "frame log entry " << index << " declares an impossible length\n";
      return 4;
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(length));
    if (stream.gcount() != static_cast<std::streamsize>(length)) {
      std::cerr << "frame log entry " << index << " is truncated\n";
      return 4;
    }
    const auto decoded = ef::decode_frame(bytes, ef::Limits::defaults());
    if (!decoded.status.ok()) {
      std::cerr << "frame log entry " << index << " is not a valid frame: " << decoded.status.to_string() << "\n";
      return 4;
    }
    ++index;
    const auto reply = client.send_raw_and_receive(bytes);
    if (!reply.ok()) {
      std::cout << "replayed-frame " << index << " type=" << ef::to_string(decoded.frame.type)
                << " transport-error=" << reply.status().to_string() << "\n";
      ++rejected;
      continue;
    }
    // Registration and claim replies carry their outcome inside the reply
    // value rather than as a transport error, so the classifier must read the
    // embedded status instead of only looking at the frame type.
    ef::Status embedded;
    bool has_embedded_status = false;
    ef::MessageType reply_type = reply.value().type;
    if (reply_type == ef::MessageType::ERROR_REPLY) {
      const auto decoded_reply = ef::decode_error_reply(reply.value().payload, ef::Limits::defaults());
      if (decoded_reply.ok()) {
        embedded = decoded_reply.value().status;
      } else {
        embedded = ef::make_error(ef::ErrorCode::PROTOCOL_ERROR, ef::ErrorStage::TRANSPORT,
                                  "malformed error reply");
      }
      has_embedded_status = true;
    } else if (reply_type == ef::MessageType::REGISTER_ACK) {
      const auto decoded_reply = ef::decode_register_reply(reply.value().payload, ef::Limits::defaults());
      if (decoded_reply.ok()) {
        embedded = decoded_reply.value().status;
        has_embedded_status = true;
      }
    } else if (reply_type == ef::MessageType::CLAIM_RESULT) {
      const auto decoded_reply = ef::decode_claim_reply(reply.value().payload, ef::Limits::defaults());
      if (decoded_reply.ok()) {
        embedded = decoded_reply.value().status;
        has_embedded_status = true;
      }
    } else if (reply_type == ef::MessageType::ACK) {
      const auto decoded_reply = ef::decode_ack(reply.value().payload, ef::Limits::defaults());
      if (decoded_reply.ok()) {
        embedded = decoded_reply.value().status;
        has_embedded_status = true;
      }
    }

    if (has_embedded_status && embedded.failed()) {
      std::cout << "replayed-frame " << index << " type=" << ef::to_string(decoded.frame.type)
                << " rejected=" << ef::to_string(embedded.code()) << " detail=" << embedded.reason() << "\n";
      ++rejected;
      continue;
    }
    std::cout << "replayed-frame " << index << " type=" << ef::to_string(decoded.frame.type)
              << " accepted reply-type=" << ef::to_string(reply_type)
              << " embedded-status=" << (has_embedded_status ? ef::to_string(embedded.code()) : std::string_view("none"))
              << "\n";
  }
  std::cout << "replayed-total " << index << " rejected " << rejected << "\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    std::string host = "127.0.0.1";
    std::uint16_t port = 0;
    int index = 1;
    for (; index < argc; ++index) {
      const std::string argument = argv[index];
      std::string value;
      if (argument == "--help" || argument == "-h") {
        print_usage();
        return 0;
      }
      if (argument == "--host") {
        if (!take_value(argc, argv, index, value)) return 2;
        host = value;
      } else if (argument == "--port") {
        if (!take_value(argc, argv, index, value)) return 2;
        port = static_cast<std::uint16_t>(std::stoul(value));
      } else {
        break;
      }
    }
    if (index >= argc) {
      print_usage();
      return 2;
    }
    const std::string command = argv[index++];

    ef::Client::Config config;
    config.host = host;
    config.port = port;
    config.limits = ef::Limits::defaults();
    auto client = ef::Client::connect(std::move(config));
    if (!client.ok()) {
      std::cerr << "cannot connect: " << client.status().to_string() << "\n";
      return 3;
    }
    ef::Client& connection = *client.value();

    if (command == "status") return print_lines(connection.query(ef::QueryKind::STATUS, ""));
    if (command == "list") return print_lines(connection.query(ef::QueryKind::LIST_EXPERIMENTS, ""));
    if (command == "workers") return print_lines(connection.query(ef::QueryKind::WORKERS, ""));
    if (command == "validate") return print_lines(connection.query(ef::QueryKind::VALIDATE_STATE, ""));

    const auto require_argument = [&](std::string& out) {
      if (index >= argc) {
        std::cerr << "command " << command << " requires an argument\n";
        return false;
      }
      out = argv[index++];
      return true;
    };

    std::string argument;
    if (command == "snapshot" || command == "hypothesis" || command == "branches" || command == "lineage" ||
        command == "trials" || command == "observations" || command == "rejected" || command == "artifacts") {
      if (!require_argument(argument)) return 2;
      const auto experiment = ef::ExperimentId::parse(argument);
      if (!experiment.has_value()) {
        std::cerr << "experiment identity must be canonical decimal text\n";
        return 2;
      }
      ef::QueryKind kind = ef::QueryKind::EXPERIMENT_SNAPSHOT;
      if (command == "hypothesis") kind = ef::QueryKind::HYPOTHESIS;
      if (command == "branches") kind = ef::QueryKind::EXPERIMENT_SNAPSHOT;
      if (command == "lineage") kind = ef::QueryKind::BRANCH_LINEAGE;
      if (command == "trials") kind = ef::QueryKind::TRIALS;
      if (command == "observations") kind = ef::QueryKind::OBSERVATIONS;
      if (command == "rejected") kind = ef::QueryKind::REJECTED_EVIDENCE;
      if (command == "artifacts") kind = ef::QueryKind::ARTIFACTS;
      return print_lines(connection.query(kind, experiment->to_string()));
    }
    if (command == "repro" || command == "explain") {
      if (!require_argument(argument)) return 2;
      const auto kind = command == "repro" ? ef::QueryKind::REPRODUCIBILITY : ef::QueryKind::EXPLAIN_DECISION;
      return print_lines(connection.query(kind, argument));
    }
    if (command == "finalize") {
      std::string experiment_text;
      std::string branch_text;
      if (!require_argument(experiment_text) || !require_argument(branch_text)) return 2;
      const auto experiment = ef::ExperimentId::parse(experiment_text);
      const auto branch = ef::BranchId::parse(branch_text);
      if (!experiment.has_value() || !branch.has_value()) {
        std::cerr << "finalize requires canonical decimal identities\n";
        return 2;
      }
      std::string reason;
      if (index < argc) {
        reason = argv[index++];
      }
      ef::FinalizeExperimentRequest request;
      request.experiment = *experiment;
      request.candidate = *branch;
      request.reason = reason;
      const auto decision = connection.finalize_experiment(request);
      if (!decision.ok()) {
        std::cerr << "finalize failed: " << decision.status().to_string() << "\n";
        return 5;
      }
      const std::vector<std::string> lines = ef::inspect::render_decision(decision.value(), ef::Limits::defaults());
      for (const std::string& line : lines) {
        std::cout << line << "\n";
      }
      return 0;
    }
    if (command == "create-trial") {
      std::string experiment_text;
      std::string branch_text;
      if (!require_argument(experiment_text) || !require_argument(branch_text)) return 2;
      const auto experiment = ef::ExperimentId::parse(experiment_text);
      const auto branch = ef::BranchId::parse(branch_text);
      if (!experiment.has_value() || !branch.has_value()) {
        std::cerr << "create-trial requires canonical decimal identities\n";
        return 2;
      }
      ef::CreateTrialRequest request;
      request.experiment = *experiment;
      request.branch = *branch;
      request.payload = index < argc ? argv[index++] : std::string();
      const auto reply = connection.request(ef::MessageType::CREATE_TRIAL, ef::encode(request, connection.limits()));
      if (!reply.ok()) {
        std::cerr << "create-trial failed: " << reply.status().to_string() << "\n";
        return 5;
      }
      const auto value = ef::decode_value_reply(reply.value().payload, connection.limits());
      if (!value.ok() || value.value().status.failed()) {
        std::cerr << "create-trial rejected\n";
        return 5;
      }
      std::cout << "trial " << value.value().value << "\n";
      return 0;
    }
    if (command == "fork") {
      std::string experiment_text;
      std::string parent_text;
      std::string name;
      if (!require_argument(experiment_text) || !require_argument(parent_text) || !require_argument(name)) return 2;
      const auto experiment = ef::ExperimentId::parse(experiment_text);
      const auto parent = ef::BranchId::parse(parent_text);
      if (!experiment.has_value() || !parent.has_value()) {
        std::cerr << "fork requires canonical decimal identities\n";
        return 2;
      }
      ef::ForkBranchRequest request;
      request.experiment = *experiment;
      request.parent = *parent;
      request.name = name;
      request.role = ef::BranchRole::CANDIDATE;
      request.planned_trials = 1;
      const auto reply = connection.request(ef::MessageType::FORK_BRANCH, ef::encode(request, connection.limits()));
      if (!reply.ok()) {
        std::cerr << "fork failed: " << reply.status().to_string() << "\n";
        return 5;
      }
      const auto value = ef::decode_value_reply(reply.value().payload, connection.limits());
      if (!value.ok() || value.value().status.failed()) {
        std::cerr << "fork rejected: " << (value.ok() ? value.value().status.to_string() : std::string("protocol")) << "\n";
        return 5;
      }
      std::cout << "branch " << value.value().value << "\n";
      return 0;
    }
    if (command == "cancel-trial") {
      if (!require_argument(argument)) return 2;
      const auto trial = ef::TrialId::parse(argument);
      if (!trial.has_value()) {
        std::cerr << "trial identity must be canonical decimal text\n";
        return 2;
      }
      ef::CancelTrialRequest request;
      request.trial = *trial;
      request.reason = index < argc ? argv[index++] : std::string("operator cancellation");
      const ef::Status status = connection.cancel_trial(request);
      std::cout << (status.ok() ? "cancelled\n" : status.to_string() + "\n");
      return status.ok() ? 0 : 5;
    }
    if (command == "cancel-experiment") {
      if (!require_argument(argument)) return 2;
      const auto experiment = ef::ExperimentId::parse(argument);
      if (!experiment.has_value()) {
        std::cerr << "experiment identity must be canonical decimal text\n";
        return 2;
      }
      ef::CancelExperimentRequest request;
      request.experiment = *experiment;
      request.reason = index < argc ? argv[index++] : std::string("operator cancellation");
      const ef::Status status = connection.cancel_experiment(request);
      std::cout << (status.ok() ? "cancelled\n" : status.to_string() + "\n");
      return status.ok() ? 0 : 5;
    }
    if (command == "rollback") {
      std::string experiment_text;
      std::string generation_text;
      if (!require_argument(experiment_text) || !require_argument(generation_text)) return 2;
      const auto experiment = ef::ExperimentId::parse(experiment_text);
      const auto generation = ef::ExperimentGeneration::parse(generation_text);
      if (!experiment.has_value() || !generation.has_value()) {
        std::cerr << "rollback requires canonical decimal identities\n";
        return 2;
      }
      ef::RollbackRequest request;
      request.experiment = *experiment;
      request.target_generation = *generation;
      request.reason = index < argc ? argv[index++] : std::string("operator rollback");
      const ef::Status status = connection.rollback(request);
      std::cout << (status.ok() ? "rolled-back\n" : status.to_string() + "\n");
      return status.ok() ? 0 : 5;
    }
    if (command == "replay") {
      if (!require_argument(argument)) return 2;
      return replay_frames(connection, argument);
    }
    if (command == "shutdown") {
      const ef::Status status = connection.shutdown();
      std::cout << (status.ok() ? "shutdown-requested\n" : status.to_string() + "\n");
      return status.ok() ? 0 : 5;
    }

    std::cerr << "unknown command: " << command << "\n";
    print_usage();
    return 2;
  } catch (const std::exception& exception) {
    std::cerr << "fatal: " << exception.what() << "\n";
    return 4;
  }
}
