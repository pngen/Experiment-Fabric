// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef EXPERIMENT_FABRIC_TRANSPORT_HPP
#define EXPERIMENT_FABRIC_TRANSPORT_HPP

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "experiment_fabric/error.hpp"
#include "experiment_fabric/limits.hpp"
#include "experiment_fabric/protocol.hpp"

namespace experiment_fabric {

/// \file
/// Bounded, framed, loopback TCP transport.
///
/// The transport never trusts a declared length: the header is validated and the
/// payload length is bounded before a single byte is allocated for it. Short
/// reads, closed sockets and malformed headers produce typed statuses.
///
/// The transport is deliberately independent of the governance layer so that
/// the coordinator can be exercised in-process without a socket, and so that
/// lock-ordering rules are unaffected by network I/O.

/// Platform socket runtime initialisation, reference counted per process.
class SocketRuntime {
 public:
  SocketRuntime();
  ~SocketRuntime();
  SocketRuntime(const SocketRuntime&) = delete;
  SocketRuntime& operator=(const SocketRuntime&) = delete;

  [[nodiscard]] bool ok() const noexcept { return ok_; }
  [[nodiscard]] static Status ensure_initialised();

 private:
  bool ok_ = false;
};

/// One connected TCP endpoint.
class TcpConnection {
 public:
  ~TcpConnection();
  TcpConnection(const TcpConnection&) = delete;
  TcpConnection& operator=(const TcpConnection&) = delete;

  /// Connects to a loopback endpoint. \c connect_timeout_ms bounds only the
  /// connection handshake, never test completion.
  [[nodiscard]] static Result<std::unique_ptr<TcpConnection>> connect(const std::string& host, std::uint16_t port);

  /// Adopts an already connected native socket.
  [[nodiscard]] static std::unique_ptr<TcpConnection> adopt(std::uintptr_t native_socket, std::string peer);

  [[nodiscard]] Status send_all(std::span<const std::uint8_t> bytes);
  [[nodiscard]] Status send_frame(const Frame& frame, const Limits& limits);

  /// Receives exactly one frame. Header fields are validated and the declared
  /// payload length is bounded before allocation.
  [[nodiscard]] Result<Frame> receive_frame(const Limits& limits);

  /// Returns NOT_FOUND when the channel reaches a clean end of stream.
  [[nodiscard]] Result<std::vector<std::uint8_t>> receive_exact(std::size_t count);

  void close();
  [[nodiscard]] bool is_open() const noexcept { return socket_ != kInvalidSocket; }
  [[nodiscard]] const std::string& peer() const noexcept { return peer_; }

  /// Enables TCP_NODELAY so governance round trips are not delayed.
  void set_no_delay(bool enabled);

 private:
  TcpConnection() = default;

  static constexpr std::uintptr_t kInvalidSocket = static_cast<std::uintptr_t>(~static_cast<std::uintptr_t>(0));

  std::uintptr_t socket_ = kInvalidSocket;
  std::string peer_;
};

/// A listening loopback socket that yields one connection at a time.
class TcpListener {
 public:
  ~TcpListener();
  TcpListener(const TcpListener&) = delete;
  TcpListener& operator=(const TcpListener&) = delete;

  /// Binds and listens. Port 0 requests an ephemeral port.
  [[nodiscard]] static Result<std::unique_ptr<TcpListener>> bind(const std::string& address, std::uint16_t port);

  /// Blocks until a connection arrives or the listener is closed. A closed
  /// listener returns NOT_FOUND, which callers treat as a stop signal.
  [[nodiscard]] Result<std::unique_ptr<TcpConnection>> accept_one();

  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
  [[nodiscard]] std::string address() const noexcept { return address_; }
  void close();
  [[nodiscard]] bool is_open() const noexcept { return socket_ != kInvalidSocket; }

 private:
  TcpListener() = default;

  static constexpr std::uintptr_t kInvalidSocket = static_cast<std::uintptr_t>(~static_cast<std::uintptr_t>(0));

  std::uintptr_t socket_ = kInvalidSocket;
  std::uint16_t port_ = 0;
  std::string address_;
};

}  // namespace experiment_fabric

#endif  // EXPERIMENT_FABRIC_TRANSPORT_HPP
