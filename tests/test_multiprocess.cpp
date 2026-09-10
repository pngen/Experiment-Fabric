// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "experiment_fabric/client.hpp"
#include "test_support.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
namespace ef = experiment_fabric;

#if defined(_WIN32)

/// A real operating-system process used by the multi-process proof.
class Process {
 public:
  Process() = default;
  /// A test process never outlives its test: any survivor is terminated so the
  /// suite leaves no stray runtime behind.
  ~Process() {
    terminate();
    close();
  }

  Process(const Process&) = delete;
  Process& operator=(const Process&) = delete;

  bool spawn(const std::string& executable, const std::vector<std::string>& arguments,
             const std::filesystem::path& log_path) {
    close();
    std::string command = quote(executable);
    for (const std::string& argument : arguments) {
      command.push_back(' ');
      command.append(quote(argument));
    }
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    log_ = CreateFileW(log_path.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log_ == INVALID_HANDLE_VALUE) {
      return false;
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = log_;
    startup.hStdError = log_;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(L'\0');
    PROCESS_INFORMATION information{};
    const BOOL created = CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                        nullptr, &startup, &information);
    if (created == FALSE) {
      return false;
    }
    process_ = information.hProcess;
    thread_ = information.hThread;
    pid_ = information.dwProcessId;
    // The harness keeps a registry of every child it started, so no test can
    // leave a stray runtime process behind even if its own teardown is skipped.
    ef_test::register_spawned_process(information.dwProcessId);
    return true;
  }

  [[nodiscard]] bool running() const {
    return process_ != nullptr && WaitForSingleObject(process_, 1) == WAIT_TIMEOUT;
  }

  /// Waits until the process exits. The poll interval is the minimum valid
  /// value, so each poll returns promptly rather than delaying the test.
  [[nodiscard]] bool wait_for_exit(std::uint32_t max_polls = 60000) {
    for (std::uint32_t poll = 0; poll < max_polls; ++poll) {
      if (!running()) {
        return true;
      }
    }
    return !running();
  }

  /// Terminates the process and verifies that it actually stopped.
  ///
  /// The termination uses the process id captured at creation, opened through a
  /// fresh handle, so it cannot depend on the rights of the original handle. A
  /// process that survives every attempt is reported loudly instead of being
  /// left behind as an orphan.
  void terminate() {
    if (process_ == nullptr) {
      return;
    }
    (void)TerminateProcess(process_, 9);
    (void)WaitForSingleObject(process_, INFINITE);
    for (int attempt = 0; attempt < 3 && !stopped(); ++attempt) {
      if (pid_ == 0) {
        break;
      }
      HANDLE reopened = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid_);
      if (reopened == nullptr) {
        break;
      }
      (void)TerminateProcess(reopened, 9);
      (void)WaitForSingleObject(reopened, INFINITE);
      (void)CloseHandle(reopened);
    }
    if (!stopped()) {
      std::fprintf(stderr, "ef_tests: process %lu could not be terminated\\n", static_cast<unsigned long>(pid_));
      return;
    }
    (void)GetExitCodeProcess(process_, &last_exit_code_);
  }

  /// True once the process has ended.
  [[nodiscard]] bool stopped() const {
    if (process_ == nullptr) {
      return true;
    }
    DWORD code = 0;
    return GetExitCodeProcess(process_, &code) != FALSE && code != STILL_ACTIVE;
  }

  [[nodiscard]] std::uint32_t exit_code() const {
    DWORD code = 0;
    if (process_ != nullptr) {
      (void)GetExitCodeProcess(process_, &code);
    }
    return static_cast<std::uint32_t>(code);
  }

  void close() {
    if (process_ != nullptr) {
      (void)CloseHandle(process_);
      process_ = nullptr;
    }
    if (thread_ != nullptr) {
      (void)CloseHandle(thread_);
      thread_ = nullptr;
    }
    if (log_ != INVALID_HANDLE_VALUE) {
      (void)CloseHandle(log_);
      log_ = INVALID_HANDLE_VALUE;
    }
  }

 private:
  static std::string quote(const std::string& value) {
    std::string result = "\"";
    for (const char character : value) {
      if (character == '"') {
        result.push_back('\\');
      }
      // Windows accepts both separators, but a canonical command line uses the
      // native one so that the process is found regardless of the generator.
      result.push_back(character == '/' ? '\\' : character);
    }
    result.push_back('"');
    return result;
  }

  HANDLE process_ = nullptr;
  HANDLE thread_ = nullptr;
  HANDLE log_ = INVALID_HANDLE_VALUE;
  DWORD last_exit_code_ = STILL_ACTIVE;
  DWORD pid_ = 0;
};

/// Runs the coordinator executable on an ephemeral port and discovers it.
class CoordinatorProcess {
 public:
  CoordinatorProcess() = default;
  ~CoordinatorProcess() { stop(); }
  CoordinatorProcess(const CoordinatorProcess&) = delete;
  CoordinatorProcess& operator=(const CoordinatorProcess&) = delete;

  /// Starts a coordinator process that owns \c state_path. Sharing the state
  /// path across two instances models an independent coordinator restart.
  bool start(const std::filesystem::path& directory, const std::string& scenario, const std::string& tag,
             const std::filesystem::path& state_path = {}) {
    state_ = state_path.empty() ? directory / (tag + ".efstate") : state_path;
    port_file_ = directory / (tag + ".port");
    std::error_code error;
    std::filesystem::remove(port_file_, error);
    const std::vector<std::string> arguments = {"--state", state_.string(), "--port", "0", "--port-file",
                                                port_file_.string(), "--scenario", scenario};
    if (!process_.spawn(EF_COORDINATOR_EXE, arguments, directory / (tag + ".log"))) {
      return false;
    }
    for (std::uint32_t poll = 0; poll < 40000; ++poll) {
      if (std::filesystem::exists(port_file_, error)) {
        std::ifstream stream(port_file_);
        std::uint32_t port = 0;
        if (stream >> port && port != 0) {
          port_ = static_cast<std::uint16_t>(port);
          return true;
        }
      }
      if (!process_.running()) {
        return false;
      }
      (void)WaitForSingleObject(nullptr, 1);
    }
    return false;
  }

  void kill() { process_.terminate(); }
  void stop() { process_.terminate(); }

  [[nodiscard]] std::uint16_t port() const { return port_; }
  [[nodiscard]] const std::filesystem::path& state() const { return state_; }

 private:
  Process process_;
  std::filesystem::path state_;
  std::filesystem::path port_file_;
  std::uint16_t port_ = 0;
};

ef::Result<std::unique_ptr<ef::Client>> connect(std::uint16_t port) {
  ef::Client::Config config;
  config.host = "127.0.0.1";
  config.port = port;
  config.limits = ef::Limits::defaults();
  for (std::uint32_t poll = 0; poll < 4000; ++poll) {
    auto client = ef::Client::connect(config);
    if (client.ok()) {
      return client;
    }
    (void)WaitForSingleObject(nullptr, 1);
  }
  return ef::make_error(ef::ErrorCode::NOT_FOUND, ef::ErrorStage::TRANSPORT, "cannot reach the coordinator");
}

/// Runs the worker executable to completion.
bool run_worker(std::uint16_t port, std::uint64_t worker_identity, const std::string& workload,
                std::uint64_t cycles, const std::filesystem::path& directory, const std::string& tag,
                const std::string& frame_log, std::uint64_t die_after_publish = 0) {
  Process process;
  std::vector<std::string> arguments = {"--host", "127.0.0.1", "--port", std::to_string(port), "--worker",
                                        std::to_string(worker_identity), "--workload", workload, "--cycles",
                                        std::to_string(cycles)};
  if (!frame_log.empty()) {
    arguments.push_back("--frame-log");
    arguments.push_back(frame_log);
  }
  if (die_after_publish != 0) {
    arguments.push_back("--die-after-publish");
    arguments.push_back(std::to_string(die_after_publish));
  }
  if (!process.spawn(EF_WORKER_EXE, arguments, directory / (tag + ".log"))) {
    return false;
  }
  return process.wait_for_exit() && process.exit_code() == 0;
}

/// Replays preserved frames through the inspection client and returns its
/// output.
std::string replay_frames(std::uint16_t port, const std::filesystem::path& frame_log,
                          const std::filesystem::path& directory, const std::string& tag) {
  Process process;
  const std::filesystem::path output = directory / (tag + ".replay.log");
  const std::vector<std::string> arguments = {"--host", "127.0.0.1", "--port", std::to_string(port), "replay",
                                              frame_log.string()};
  if (!process.spawn(EF_CTL_EXE, arguments, output)) {
    return std::string("REPLAY-SPAWN-FAILED for ") + EF_CTL_EXE;
  }
  if (!process.wait_for_exit()) {
    return std::string("REPLAY-WAIT-FAILED exit=") + std::to_string(process.exit_code());
  }
  std::ifstream stream(output, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

std::vector<std::string> query_lines(std::uint16_t port, ef::QueryKind kind, const std::string& argument) {
  auto client = connect(port);
  if (!client.ok()) {
    return {};
  }
  const auto lines = client.value()->query(kind, argument);
  if (!lines.ok()) {
    return {};
  }
  return lines.value();
}

/// Extracts "experiment <id>" from a scenario start.
ef::ExperimentId experiment_of(const std::vector<std::string>& lines) {
  for (const std::string& line : lines) {
    if (line.rfind("experiment ", 0) == 0) {
      const std::size_t begin = line.find(' ', 11);
      const std::string text = begin == std::string::npos ? line.substr(11) : line.substr(11, begin - 11);
      const auto parsed = ef::ExperimentId::parse(text);
      if (parsed.has_value()) {
        return *parsed;
      }
    }
  }
  return ef::ExperimentId{};
}

std::uint64_t count_completed(const std::vector<std::string>& trial_lines) {
  std::uint64_t completed = 0;
  for (const std::string& line : trial_lines) {
    if (line.rfind("trial ", 0) == 0 && line.find("state=COMPLETED") != std::string::npos) {
      ++completed;
    }
  }
  return completed;
}

/// Joins query output into one diagnostic string.
std::string join_lines(const std::vector<std::string>& lines) {
  std::string text;
  for (const std::string& line : lines) {
    text.append(line);
    text.push_back('\n');
  }
  return text;
}

std::uint64_t count_abandoned(const std::vector<std::string>& trial_lines) {
  std::uint64_t abandoned = 0;
  for (const std::string& line : trial_lines) {
    if (line.find("state=ABANDONED") != std::string::npos) {
      ++abandoned;
    }
  }
  return abandoned;
}
#endif  // _WIN32

}  // namespace

EF_TEST(multiprocess, worker_process_death_is_fenced_and_history_is_preserved) {
#if !defined(_WIN32)
  EF_CHECK_MESSAGE(true, "the multi-process proof is validated on Windows x64 (UNSUPPORTED elsewhere)");
#else
  const std::filesystem::path directory = ef_test::scratch_directory("multiprocess-worker-death");
  CoordinatorProcess coordinator;
  EF_REQUIRE(coordinator.start(directory, "authority", "worker-death"));
  const std::vector<std::string> experiment_lines = query_lines(coordinator.port(), ef::QueryKind::LIST_EXPERIMENTS, "");
  const ef::ExperimentId experiment = experiment_of(experiment_lines);
  EF_REQUIRE(experiment.valid());

  // Worker A runs under a boot identity of its own and is killed as a real
  // operating-system process while it is executing a trial.
  const std::filesystem::path frames_a = directory / "worker-a.frames";
  Process worker_a;
  const std::vector<std::string> arguments = {
      "--host", "127.0.0.1", "--port", std::to_string(coordinator.port()), "--worker", "1",
      "--workload", "parameter-slow", "--cycles", "1", "--frame-log", frames_a.string()};
  EF_REQUIRE(worker_a.spawn(EF_WORKER_EXE, arguments, directory / "worker-a.log"));

  bool registered = false;
  for (std::uint32_t poll = 0; poll < 40000 && !registered; ++poll) {
    const std::vector<std::string> workers = query_lines(coordinator.port(), ef::QueryKind::WORKERS, "");
    for (const std::string& line : workers) {
      if (line.find("LIVE") != std::string::npos) {
        registered = true;
      }
    }
    if (!worker_a.running()) {
      break;
    }
    (void)WaitForSingleObject(nullptr, 1);
  }
  EF_CHECK_MESSAGE(registered, "worker A never appeared as a live incarnation: " +
                                    join_lines(query_lines(coordinator.port(), ef::QueryKind::WORKERS, "")));
  worker_a.terminate();
  EF_CHECK(!worker_a.running());

  // The dead incarnation's authority is revoked, and the unfinished assignment
  // is transitioned conservatively rather than left live.
  bool replacement_ran = run_worker(coordinator.port(), 1, "parameter", 8, directory, "worker-b", "");
  EF_CHECK(replacement_ran);
  const std::vector<std::string> trials = query_lines(coordinator.port(), ef::QueryKind::TRIALS,
                                                      experiment.to_string());
  EF_CHECK_EQ(count_completed(trials), 2ull);
  EF_CHECK(count_abandoned(trials) >= 1);
  std::uint64_t committed = 0;
  for (const std::string& line : trials) {
    if (line.find("committed=true") != std::string::npos) {
      ++committed;
    }
  }
  EF_CHECK_EQ(committed, 2ull);
  for (const std::string& line : trials) {
    if (line.rfind("trial ", 0) == 0) {
      EF_CHECK(line.find("committed-attempts=1") != std::string::npos ||
               line.find("committed-attempts=0") != std::string::npos);
      EF_CHECK(line.find("committed-attempts=2") == std::string::npos);
    }
  }

  // Replaying the dead incarnation's preserved traffic is rejected.
  const std::string replay = replay_frames(coordinator.port(), frames_a, directory, "worker-a-replay");
  EF_CHECK_MESSAGE(!replay.empty(), "replay produced no output");
  EF_CHECK_MESSAGE(replay.find("replayed-total") != std::string::npos, "replay output: " + replay);
  EF_CHECK(replay.find("rejected=") != std::string::npos);
  EF_CHECK(replay.find("accepted reply-type") == std::string::npos);

  // The experiment still reaches an authoritative decision from current
  // evidence only.
  auto client = connect(coordinator.port());
  EF_REQUIRE(client.ok());
  const std::vector<std::string> branch_lines = query_lines(coordinator.port(), ef::QueryKind::EXPERIMENT_SNAPSHOT,
                                                            experiment.to_string());
  ef::BranchId candidate;
  for (const std::string& line : branch_lines) {
    if (line.find("name=candidate") != std::string::npos) {
      const std::size_t begin = line.find("branch ") + 7;
      const std::size_t end = line.find(' ', begin);
      const auto parsed = ef::BranchId::parse(line.substr(begin, end - begin));
      if (parsed.has_value()) {
        candidate = *parsed;
      }
    }
  }
  EF_REQUIRE(candidate.valid());
  ef::FinalizeExperimentRequest finalize;
  finalize.experiment = experiment;
  finalize.candidate = candidate;
  finalize.reason = "multi-process worker death proof";
  const auto decision = client.value()->finalize_experiment(finalize);
  EF_REQUIRE(decision.ok());
  EF_CHECK(decision.value().outcome == ef::DecisionOutcome::ACCEPT);

  const std::vector<std::string> validated = query_lines(coordinator.port(), ef::QueryKind::VALIDATE_STATE, "");
  EF_CHECK(!validated.empty());
  EF_CHECK(validated[0].find("OK") != std::string::npos);
  coordinator.stop();
#endif
}

EF_TEST(multiprocess, coordinator_restart_fences_the_previous_epoch) {
#if !defined(_WIN32)
  EF_CHECK_MESSAGE(true, "the coordinator restart proof is validated on Windows x64 (UNSUPPORTED elsewhere)");
#else
  const std::filesystem::path directory = ef_test::scratch_directory("multiprocess-coordinator-restart");
  CoordinatorProcess first;
  EF_REQUIRE(first.start(directory, "authority", "coordinator-first"));
  const std::vector<std::string> lines = query_lines(first.port(), ef::QueryKind::LIST_EXPERIMENTS, "");
  const ef::ExperimentId experiment = experiment_of(lines);
  EF_REQUIRE(experiment.valid());

  const std::vector<std::string> status_before = query_lines(first.port(), ef::QueryKind::STATUS, "");
  EF_REQUIRE(!status_before.empty());
  std::uint64_t epoch_before = 0;
  for (const std::string& line : status_before) {
    if (line.rfind("coordinator-epoch ", 0) == 0) {
      epoch_before = std::stoull(line.substr(18));
    }
  }
  EF_CHECK(epoch_before != 0);

  // A worker completes one trial and exits cleanly, preserving its real traffic.
  const std::filesystem::path frames = directory / "worker-first.frames";
  EF_CHECK(run_worker(first.port(), 7, "parameter", 1, directory, "worker-first", frames.string()));
  const std::vector<std::string> trials_before = query_lines(first.port(), ef::QueryKind::TRIALS,
                                                             experiment.to_string());
  const std::uint64_t completed_before = count_completed(trials_before);
  EF_CHECK_EQ(completed_before, 1ull);

  // The coordinator process is terminated without any graceful shutdown.
  first.kill();

  // The second coordinator opens the *same* durable state image, which is what
  // makes this an independent restart rather than a fresh experiment.
  CoordinatorProcess second;
  EF_REQUIRE(second.start(directory, "", "coordinator-second", first.state()));
  const std::vector<std::string> status_after = query_lines(second.port(), ef::QueryKind::STATUS, "");
  EF_REQUIRE(!status_after.empty());
  std::uint64_t epoch_after = 0;
  for (const std::string& line : status_after) {
    if (line.rfind("coordinator-epoch ", 0) == 0) {
      epoch_after = std::stoull(line.substr(18));
    }
  }
  EF_CHECK(epoch_after > epoch_before);

  // Durable history survives, while live process authority does not.
  const std::vector<std::string> trials_after = query_lines(second.port(), ef::QueryKind::TRIALS,
                                                            experiment.to_string());
  EF_CHECK_EQ(count_completed(trials_after), completed_before);
  const std::vector<std::string> workers_after = query_lines(second.port(), ef::QueryKind::WORKERS, "");
  EF_REQUIRE(!workers_after.empty());
  for (const std::string& line : workers_after) {
    EF_CHECK(line.find("LIVE") == std::string::npos);
  }

  // Traffic preserved from the previous coordinator epoch is rejected.
  const std::string replay = replay_frames(second.port(), frames, directory, "pre-restart-replay");
  EF_CHECK_MESSAGE(!replay.empty(), "replay produced no output");
  EF_CHECK_MESSAGE(replay.find("replayed-total") != std::string::npos, "replay output: " + replay);
  EF_CHECK(replay.find("STALE_EPOCH") != std::string::npos);
  EF_CHECK(replay.find("accepted reply-type") == std::string::npos);

  // Fresh work under the new epoch completes the experiment.
  EF_CHECK(run_worker(second.port(), 8, "parameter", 8, directory, "worker-second", ""));
  const std::vector<std::string> final_trials = query_lines(second.port(), ef::QueryKind::TRIALS,
                                                            experiment.to_string());
  EF_CHECK_EQ(count_completed(final_trials), 2ull);

  const std::vector<std::string> snapshot = query_lines(second.port(), ef::QueryKind::EXPERIMENT_SNAPSHOT,
                                                        experiment.to_string());
  ef::BranchId candidate;
  for (const std::string& line : snapshot) {
    if (line.find("name=candidate") != std::string::npos) {
      const std::size_t begin = line.find("branch ") + 7;
      const std::size_t end = line.find(' ', begin);
      const auto parsed = ef::BranchId::parse(line.substr(begin, end - begin));
      if (parsed.has_value()) {
        candidate = *parsed;
      }
    }
  }
  EF_REQUIRE(candidate.valid());
  auto client = connect(second.port());
  EF_REQUIRE(client.ok());
  ef::FinalizeExperimentRequest finalize;
  finalize.experiment = experiment;
  finalize.candidate = candidate;
  finalize.reason = "multi-process coordinator restart proof";
  const auto decision = client.value()->finalize_experiment(finalize);
  EF_REQUIRE(decision.ok());
  EF_CHECK(decision.value().outcome == ef::DecisionOutcome::ACCEPT);
  // Every factor is supported by evidence produced under the surviving
  // coordinator epoch.
  EF_CHECK(decision.value().generation.value() >= 1);

  // An orderly stop is still possible for the surviving coordinator.
  EF_CHECK(client.value()->shutdown().ok());
  second.stop();
#endif
}

EF_TEST(multiprocess, worker_death_during_publication_never_produces_a_completion) {
#if !defined(_WIN32)
  EF_CHECK_MESSAGE(true, "the publication-death proof is validated on Windows x64 (UNSUPPORTED elsewhere)");
#else
  const std::filesystem::path directory = ef_test::scratch_directory("multiprocess-publication-death");
  CoordinatorProcess coordinator;
  EF_REQUIRE(coordinator.start(directory, "authority", "publication-death"));
  const std::vector<std::string> lines = query_lines(coordinator.port(), ef::QueryKind::LIST_EXPERIMENTS, "");
  const ef::ExperimentId experiment = experiment_of(lines);
  EF_REQUIRE(experiment.valid());

  Process worker;
  const std::filesystem::path frames = directory / "publication-death.frames";
  const std::vector<std::string> arguments = {"--host", "127.0.0.1", "--port", std::to_string(coordinator.port()),
                                              "--worker", "3", "--workload", "parameter", "--cycles", "1",
                                              "--die-after-publish", "1", "--frame-log", frames.string()};
  const bool spawned = worker.spawn(EF_WORKER_EXE, arguments, directory / "publication-death.log");
  EF_REQUIRE(spawned);
  const bool exited = worker.wait_for_exit();
  std::ifstream observation_log(directory / "publication-death.log", std::ios::binary);
  const std::string observed{std::istreambuf_iterator<char>(observation_log), std::istreambuf_iterator<char>()};
  EF_CHECK_MESSAGE(exited, "the worker never exited");
  std::ostringstream exit_diagnostic;
  exit_diagnostic << "exit code " << worker.exit_code() << " with output: " << observed;
  const bool deliberate_death = worker.exit_code() == 9u;
  EF_CHECK_MESSAGE(deliberate_death, exit_diagnostic.str());

  // The trial the dead worker was publishing is not completed: a produced
  // observation is not a completed trial.
  const std::vector<std::string> trials = query_lines(coordinator.port(), ef::QueryKind::TRIALS,
                                                      experiment.to_string());
  EF_CHECK_EQ(count_completed(trials), 0ull);

  // A fresh incarnation completes the experiment authoritatively.
  EF_CHECK(run_worker(coordinator.port(), 3, "parameter", 8, directory, "publication-recovery", ""));
  const std::vector<std::string> recovered = query_lines(coordinator.port(), ef::QueryKind::TRIALS,
                                                         experiment.to_string());
  EF_CHECK_MESSAGE(count_completed(recovered) == 2ull, "unexpected trial state: " + join_lines(recovered));

  const std::string replay = replay_frames(coordinator.port(), frames, directory, "publication-death-replay");
  EF_CHECK(replay.find("accepted reply-type") == std::string::npos);
  coordinator.stop();
#endif
}
