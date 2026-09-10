// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

#include "coordinator_internal.hpp"

namespace experiment_fabric {
namespace {

Frame make_error_frame(std::uint64_t correlation, const Status& status, const Limits& limits) {
  Frame frame;
  frame.type = MessageType::ERROR_REPLY;
  frame.correlation = correlation;
  ErrorReply reply;
  reply.status = status;
  reply.detail = status.to_string();
  frame.payload = encode(reply, limits);
  return frame;
}

Frame make_ack_frame(std::uint64_t correlation, const Status& status, const Limits& limits) {
  Frame frame;
  frame.type = MessageType::ACK;
  frame.correlation = correlation;
  AckReply ack;
  ack.status = status;
  frame.payload = encode(ack, limits);
  return frame;
}

}  // namespace

/// Server-side connection queue and fixed thread pool.
///
/// Threads are bounded by configuration: no thread is created per connection on
/// demand beyond the pool, and none per request or per experiment. The pool
/// never touches coordinator state directly; every request is handed to the
/// same public governance API an in-process caller would use, so authority
/// rules cannot differ between local and networked callers.
class CoordinatorServerRuntime {
 public:
  CoordinatorServerRuntime(Coordinator& coordinator, Limits limits)
      : coordinator_(coordinator), limits_(limits) {}

  ~CoordinatorServerRuntime() { stop(); }

  Status start(std::uint16_t port, const std::string& address, std::uint32_t thread_count) {
    auto listener = TcpListener::bind(address, port);
    if (!listener.ok()) {
      return listener.status();
    }
    listener_ = std::move(listener.value());
    const std::uint32_t bounded =
        std::max<std::uint32_t>(1, std::min<std::uint32_t>(thread_count, limits_.max_worker_threads));
    for (std::uint32_t index = 0; index < bounded; ++index) {
      pool_.emplace_back([this]() { worker_loop(); });
    }
    accept_thread_ = std::thread([this]() { accept_loop(); });
    return Status::success();
  }

  [[nodiscard]] std::uint16_t port() const { return listener_ == nullptr ? 0 : listener_->port(); }

  void stop() {
    if (stopping_.exchange(true)) {
      return;
    }
    if (listener_ != nullptr) {
      listener_->close();
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (TcpConnection* connection : active_) {
        connection->close();
      }
      pending_.clear();
    }
    available_.notify_all();
    if (accept_thread_.joinable()) {
      accept_thread_.join();
    }
    for (std::thread& thread : pool_) {
      if (thread.joinable()) {
        thread.join();
      }
    }
    pool_.clear();
    listener_.reset();
  }

 private:
  void accept_loop() {
    while (!stopping_.load()) {
      auto connection = listener_->accept_one();
      if (!connection.ok()) {
        return;
      }
      connection.value()->set_no_delay(true);
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_.load()) {
          connection.value()->close();
          return;
        }
        pending_.push_back(std::move(connection.value()));
      }
      available_.notify_one();
    }
  }

  void worker_loop() {
    while (true) {
      std::unique_ptr<TcpConnection> connection;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        available_.wait(lock, [this]() { return stopping_.load() || !pending_.empty(); });
        if (pending_.empty()) {
          return;
        }
        connection = std::move(pending_.front());
        pending_.pop_front();
        active_.push_back(connection.get());
      }
      serve(*connection);
      {
        std::lock_guard<std::mutex> lock(mutex_);
        active_.erase(std::remove(active_.begin(), active_.end(), connection.get()), active_.end());
      }
      connection->close();
    }
  }

  void serve(TcpConnection& connection) {
    while (!stopping_.load()) {
      auto frame = connection.receive_frame(limits_);
      if (!frame.ok()) {
        return;
      }
      Frame reply = dispatch(frame.value());
      const Status sent = connection.send_frame(reply, limits_);
      if (!sent.ok()) {
        return;
      }
    }
  }

  Frame dispatch(const Frame& request);

  Coordinator& coordinator_;
  Limits limits_;
  std::unique_ptr<TcpListener> listener_;
  std::thread accept_thread_;
  std::vector<std::thread> pool_;
  std::mutex mutex_;
  std::condition_variable available_;
  std::deque<std::unique_ptr<TcpConnection>> pending_;
  std::vector<TcpConnection*> active_;
  std::atomic<bool> stopping_{false};
};

Frame CoordinatorServerRuntime::dispatch(const Frame& request) {
  Frame reply;
  reply.correlation = request.correlation;
  const std::span<const std::uint8_t> payload(request.payload);

  const auto fail = [&reply, &request, this](const Status& status) {
    reply = make_error_frame(request.correlation, status, limits_);
  };
  const auto acknowledge = [&reply, &request, this](const Status& status) {
    reply = make_ack_frame(request.correlation, status, limits_);
  };

  switch (request.type) {
    case MessageType::HELLO: {
      reply.type = MessageType::HELLO_ACK;
      ValueReply value;
      value.status = Status::success();
      value.value = coordinator_.epoch().value();
      reply.payload = encode(value, limits_);
      return reply;
    }
    case MessageType::REGISTER_WORKER: {
      auto decoded = decode_register_worker(payload, limits_);
      if (!decoded.ok()) {
        fail(decoded.status());
        return reply;
      }
      auto result = coordinator_.register_worker(decoded.value());
      if (!result.ok()) {
        fail(result.status());
        return reply;
      }
      reply.type = MessageType::REGISTER_ACK;
      const RegisterWorkerReply registration = result.value();
      reply.payload = encode(registration, limits_);
      return reply;
    }
    case MessageType::CLAIM_TRIAL: {
      auto decoded = decode_claim_trial(payload, limits_);
      if (!decoded.ok()) {
        fail(decoded.status());
        return reply;
      }
      auto result = coordinator_.claim_trials(decoded.value());
      if (!result.ok()) {
        fail(result.status());
        return reply;
      }
      reply.type = MessageType::CLAIM_RESULT;
      const ClaimTrialReply claims = result.value();
      reply.payload = encode(claims, limits_);
      return reply;
    }
    case MessageType::PUBLISH_OBSERVATION: {
      auto decoded = decode_publish_observation(payload, limits_);
      if (!decoded.ok()) {
        fail(decoded.status());
        return reply;
      }
      const Status status = coordinator_.publish_observation(decoded.value());
      if (status.failed()) {
        fail(status);
        return reply;
      }
      acknowledge(status);
      return reply;
    }
    case MessageType::PUBLISH_ARTIFACT: {
      auto decoded = decode_publish_artifact(payload, limits_);
      if (!decoded.ok()) {
        fail(decoded.status());
        return reply;
      }
      const Status status = coordinator_.publish_artifact(decoded.value());
      if (status.failed()) {
        fail(status);
        return reply;
      }
      acknowledge(status);
      return reply;
    }
    case MessageType::COMMIT_TRIAL: {
      auto decoded = decode_commit_trial(payload, limits_);
      if (!decoded.ok()) {
        fail(decoded.status());
        return reply;
      }
      const Status status = coordinator_.commit_trial(decoded.value());
      if (status.failed()) {
        fail(status);
        return reply;
      }
      acknowledge(status);
      return reply;
    }
    case MessageType::FAIL_TRIAL: {
      auto decoded = decode_fail_trial(payload, limits_);
      if (!decoded.ok()) {
        fail(decoded.status());
        return reply;
      }
      const Status status = coordinator_.fail_trial(decoded.value());
      if (status.failed()) {
        fail(status);
        return reply;
      }
      acknowledge(status);
      return reply;
    }
    case MessageType::HEARTBEAT: {
      auto decoded = decode_heartbeat(payload, limits_);
      if (!decoded.ok()) {
        fail(decoded.status());
        return reply;
      }
      const Status status = coordinator_.heartbeat(decoded.value());
      if (status.failed()) {
        fail(status);
        return reply;
      }
      acknowledge(status);
      return reply;
    }
    case MessageType::CANCEL_TRIAL: {
      auto decoded = decode_cancel_trial(payload, limits_);
      if (!decoded.ok()) {
        fail(decoded.status());
        return reply;
      }
      const Status status = coordinator_.cancel_trial(decoded.value().trial, decoded.value().reason);
      if (status.failed()) {
        fail(status);
        return reply;
      }
      acknowledge(status);
      return reply;
    }
    case MessageType::CANCEL_EXPERIMENT: {
      auto decoded = decode_cancel_experiment(payload, limits_);
      if (!decoded.ok()) {
        fail(decoded.status());
        return reply;
      }
      const Status status = coordinator_.cancel_experiment(decoded.value().experiment, decoded.value().reason);
      if (status.failed()) {
        fail(status);
        return reply;
      }
      acknowledge(status);
      return reply;
    }
    case MessageType::ROLLBACK: {
      auto decoded = decode_rollback(payload, limits_);
      if (!decoded.ok()) {
        fail(decoded.status());
        return reply;
      }
      const Status status = coordinator_.rollback(decoded.value().experiment,
                                                  decoded.value().target_generation, decoded.value().reason);
      if (status.failed()) {
        fail(status);
        return reply;
      }
      acknowledge(status);
      return reply;
    }
    case MessageType::CREATE_TRIAL: {
      auto decoded = decode_create_trial(payload, limits_);
      if (!decoded.ok()) {
        fail(decoded.status());
        return reply;
      }
      auto result =
          coordinator_.create_trial(decoded.value().experiment, decoded.value().branch, decoded.value().payload);
      if (!result.ok()) {
        fail(result.status());
        return reply;
      }
      reply.type = MessageType::CREATE_TRIAL;
      ValueReply value;
      value.status = Status::success();
      value.value = result.value().value();
      reply.payload = encode(value, limits_);
      return reply;
    }
    case MessageType::FORK_BRANCH: {
      auto decoded = decode_fork_branch(payload, limits_);
      if (!decoded.ok()) {
        fail(decoded.status());
        return reply;
      }
      BranchSpec spec;
      spec.name = decoded.value().name;
      spec.role = decoded.value().role;
      spec.parameters = decoded.value().parameters;
      spec.seed = decoded.value().seed;
      spec.planned_trials = decoded.value().planned_trials;
      auto result = coordinator_.fork_branch(decoded.value().experiment, decoded.value().parent, spec);
      if (!result.ok()) {
        fail(result.status());
        return reply;
      }
      reply.type = MessageType::FORK_BRANCH;
      ValueReply value;
      value.status = Status::success();
      value.value = result.value().value();
      reply.payload = encode(value, limits_);
      return reply;
    }
    case MessageType::REVISE_HYPOTHESIS: {
      auto decoded = decode_revise_hypothesis(payload, limits_);
      if (!decoded.ok()) {
        fail(decoded.status());
        return reply;
      }
      HypothesisRevisionSpec spec;
      spec.statement = decoded.value().statement;
      spec.provenance = decoded.value().provenance;
      if (decoded.value().has_direction) {
        spec.expected_direction = decoded.value().direction;
      }
      spec.expected_metric = decoded.value().metric;
      auto result = coordinator_.revise_hypothesis(decoded.value().experiment, spec);
      if (!result.ok()) {
        fail(result.status());
        return reply;
      }
      reply.type = MessageType::REVISE_HYPOTHESIS;
      ValueReply value;
      value.status = Status::success();
      value.value = result.value().value();
      reply.payload = encode(value, limits_);
      return reply;
    }
    case MessageType::FINALIZE_EXPERIMENT: {
      auto decoded = decode_finalize_experiment(payload, limits_);
      if (!decoded.ok()) {
        fail(decoded.status());
        return reply;
      }
      auto result = coordinator_.finalize_experiment(decoded.value().experiment, decoded.value().candidate,
                                                     decoded.value().reason);
      if (!result.ok()) {
        fail(result.status());
        return reply;
      }
      reply.type = MessageType::FINALIZE_EXPERIMENT;
      const Decision decision = result.value();
      reply.payload = encode_decision_reply(Status::success(), decision, limits_);
      return reply;
    }
    case MessageType::QUERY: {
      auto decoded = decode_query_request(payload, limits_);
      if (!decoded.ok()) {
        fail(decoded.status());
        return reply;
      }
      QueryResult result;
      const auto lines = coordinator_.query_lines(decoded.value().first, decoded.value().second);
      if (lines.ok()) {
        result.status = Status::success();
        result.lines = lines.value();
      } else {
        result.status = lines.status();
      }
      reply.type = MessageType::QUERY_RESULT;
      reply.payload = encode(result, limits_);
      return reply;
    }
    case MessageType::SHUTDOWN: {
      acknowledge(Status::success());
      coordinator_.request_shutdown();
      return reply;
    }
    case MessageType::INVALID:
    case MessageType::HELLO_ACK:
    case MessageType::REGISTER_ACK:
    case MessageType::CLAIM_RESULT:
    case MessageType::ACK:
    case MessageType::CREATE_EXPERIMENT:
    case MessageType::QUERY_RESULT:
    case MessageType::ERROR_REPLY:
    case MessageType::CREATE_HYPOTHESIS:
      break;
  }
  return make_error_frame(request.correlation,
                          make_error(ErrorCode::UNSUPPORTED, ErrorStage::TRANSPORT,
                                     std::string("message type is not supported by the coordinator: ") +
                                         std::string(to_string(request.type))),
                          limits_);
}

Coordinator::Impl::~Impl() { delete server; }

void Coordinator::request_shutdown() {
  Impl& impl = *impl_;
  {
    std::unique_lock<std::shared_mutex> lock(impl.mutex);
    impl.admitting_work = false;
  }
  {
    std::lock_guard<std::mutex> guard(impl.shutdown_mutex);
    impl.shutdown_flagged = true;
  }
  impl.shutdown_signal.notify_all();
}

void Coordinator::wait_for_shutdown_request() {
  Impl& impl = *impl_;
  std::unique_lock<std::mutex> guard(impl.shutdown_mutex);
  impl.shutdown_signal.wait(guard, [&impl]() { return impl.shutdown_flagged; });
}

bool Coordinator::shutdown_requested() const noexcept {
  std::lock_guard<std::mutex> guard(impl_->shutdown_mutex);
  return impl_->shutdown_flagged;
}

Status Coordinator::start_server() {
  Impl& impl = *impl_;
  if (impl.server_active.load()) {
    return make_error(ErrorCode::ALREADY_EXISTS, ErrorStage::TRANSPORT, "the control plane is already running");
  }
  auto* runtime = new CoordinatorServerRuntime(*this, impl.limits);
  const Status started =
      runtime->start(impl.config.listen_port, impl.config.bind_address, impl.config.max_connection_threads);
  if (!started.ok()) {
    return started;
  }
  {
    std::unique_lock<std::shared_mutex> lock(impl.mutex);
    impl.admitting_work = true;
    impl.server_active.store(true);
  }
  impl.server = runtime;
  return Status::success();
}

Status Coordinator::stop_server() {
  Impl& impl = *impl_;
  if (impl.server == nullptr) {
    return Status::success();
  }
  {
    std::unique_lock<std::shared_mutex> lock(impl.mutex);
    impl.admitting_work = false;
  }
  impl.server->stop();
  delete impl.server;
  impl.server = nullptr;
  {
    std::unique_lock<std::shared_mutex> lock(impl.mutex);
    impl.server_active.store(false);
  }
  const Status persisted = save();
  if (!persisted.ok()) {
    return persisted;
  }
  impl.notify_event("server-stopped");
  return Status::success();
}

std::uint16_t Coordinator::port() const noexcept {
  return impl_->server == nullptr ? 0 : impl_->server->port();
}

}  // namespace experiment_fabric
