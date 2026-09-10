// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "experiment_fabric/transport.hpp"

#include <cstring>
#include <mutex>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace experiment_fabric {
namespace {

Status transport_error(ErrorCode code, std::string reason) {
  return make_error(code, ErrorStage::TRANSPORT, std::move(reason));
}

#if defined(_WIN32)
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidNative = INVALID_SOCKET;

void close_native(NativeSocket socket) { ::closesocket(socket); }

int last_error() { return ::WSAGetLastError(); }

bool would_block(int error) { return error == WSAEWOULDBLOCK || error == WSAEINTR; }

#else
using NativeSocket = int;
constexpr NativeSocket kInvalidNative = -1;

void close_native(NativeSocket socket) { ::close(socket); }

int last_error() { return errno; }

bool would_block(int error) { return error == EAGAIN || error == EWOULDBLOCK || error == EINTR; }

#endif

std::once_flag g_runtime_once;
bool g_runtime_ok = false;

void initialise_sockets() {
#if defined(_WIN32)
  WSADATA data{};
  g_runtime_ok = ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
#else
  g_runtime_ok = true;
#endif
}

void shutdown_sockets() {
#if defined(_WIN32)
  ::WSACleanup();
#endif
}

}  // namespace

SocketRuntime::SocketRuntime() {
  std::call_once(g_runtime_once, initialise_sockets);
  ok_ = g_runtime_ok;
}

SocketRuntime::~SocketRuntime() {
  // The runtime is intentionally process scoped: a per-object teardown would
  // close the socket subsystem while other sessions are still active.
}

Status SocketRuntime::ensure_initialised() {
  std::call_once(g_runtime_once, initialise_sockets);
  if (!g_runtime_ok) {
    return transport_error(ErrorCode::UNSUPPORTED, "the platform socket runtime could not be initialised");
  }
  return Status::success();
}

TcpConnection::~TcpConnection() { close(); }

std::unique_ptr<TcpConnection> TcpConnection::adopt(std::uintptr_t native_socket, std::string peer) {
  std::unique_ptr<TcpConnection> connection(new TcpConnection());
  connection->socket_ = native_socket;
  connection->peer_ = std::move(peer);
  return connection;
}

Result<std::unique_ptr<TcpConnection>> TcpConnection::connect(const std::string& host, std::uint16_t port) {
  const Status ready = SocketRuntime::ensure_initialised();
  if (!ready.ok()) {
    return ready;
  }
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  const std::string service = std::to_string(port);
  addrinfo* results = nullptr;
  if (::getaddrinfo(host.c_str(), service.c_str(), &hints, &results) != 0) {
    return transport_error(ErrorCode::NOT_FOUND, "cannot resolve the coordinator endpoint");
  }
  NativeSocket socket = kInvalidNative;
  for (addrinfo* candidate = results; candidate != nullptr; candidate = candidate->ai_next) {
    socket = ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
    if (socket == kInvalidNative) {
      continue;
    }
    if (::connect(socket, candidate->ai_addr, static_cast<int>(candidate->ai_addrlen)) == 0) {
      break;
    }
    close_native(socket);
    socket = kInvalidNative;
  }
  ::freeaddrinfo(results);
  if (socket == kInvalidNative) {
    return transport_error(ErrorCode::NOT_FOUND, "cannot connect to the coordinator endpoint");
  }
  return adopt(static_cast<std::uintptr_t>(socket), host + ":" + service);
}

void TcpConnection::set_no_delay(bool enabled) {
  if (socket_ == kInvalidSocket) {
    return;
  }
  const int value = enabled ? 1 : 0;
  ::setsockopt(static_cast<NativeSocket>(socket_), IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<const char*>(&value), sizeof(value));
}

void TcpConnection::close() {
  if (socket_ == kInvalidSocket) {
    return;
  }
  const NativeSocket socket = static_cast<NativeSocket>(socket_);
  socket_ = kInvalidSocket;
  ::shutdown(socket, 2);
  close_native(socket);
}

Status TcpConnection::send_all(std::span<const std::uint8_t> bytes) {
  if (socket_ == kInvalidSocket) {
    return transport_error(ErrorCode::NOT_FOUND, "cannot send on a closed connection");
  }
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const std::size_t remaining = bytes.size() - offset;
    const int chunk = static_cast<int>(remaining > 0x7FFFFFFFu ? 0x7FFFFFFFu : remaining);
    const int sent = ::send(static_cast<NativeSocket>(socket_),
                            reinterpret_cast<const char*>(bytes.data() + offset), chunk, 0);
    if (sent <= 0) {
      const int error = last_error();
      if (would_block(error)) {
        continue;
      }
      return transport_error(ErrorCode::TRANSPORT_FAILURE, "send failed");
    }
    offset += static_cast<std::size_t>(sent);
  }
  return Status::success();
}

Status TcpConnection::send_frame(const Frame& frame, const Limits& limits) {
  auto bytes = encode_frame(frame, limits);
  if (!bytes.ok()) {
    return bytes.status();
  }
  return send_all(bytes.value());
}

Result<std::vector<std::uint8_t>> TcpConnection::receive_exact(std::size_t count) {
  if (socket_ == kInvalidSocket) {
    return transport_error(ErrorCode::NOT_FOUND, "cannot receive on a closed connection");
  }
  std::vector<std::uint8_t> buffer(count);
  std::size_t offset = 0;
  while (offset < count) {
    const std::size_t remaining = count - offset;
    const int chunk = static_cast<int>(remaining > 0x7FFFFFFFu ? 0x7FFFFFFFu : remaining);
    const int received = ::recv(static_cast<NativeSocket>(socket_),
                                reinterpret_cast<char*>(buffer.data() + offset), chunk, 0);
    if (received == 0) {
      if (offset == 0) {
        return transport_error(ErrorCode::NOT_FOUND, "peer closed the connection");
      }
      return transport_error(ErrorCode::PROTOCOL_ERROR, "connection closed mid-frame");
    }
    if (received < 0) {
      const int error = last_error();
      if (would_block(error)) {
        continue;
      }
      return transport_error(ErrorCode::TRANSPORT_FAILURE, "receive failed");
    }
    offset += static_cast<std::size_t>(received);
  }
  return buffer;
}

Result<Frame> TcpConnection::receive_frame(const Limits& limits) {
  auto header = receive_exact(kFrameHeaderBytes);
  if (!header.ok()) {
    return header.status();
  }
  auto info = parse_frame_header(header.value(), limits);
  if (!info.ok()) {
    return info.status();
  }
  Frame frame;
  frame.version = info.value().version;
  frame.type = info.value().type;
  frame.correlation = info.value().correlation;
  if (info.value().payload_length != 0) {
    auto payload = receive_exact(info.value().payload_length);
    if (!payload.ok()) {
      return payload.status();
    }
    if (crc32(payload.value()) != info.value().payload_crc) {
      return transport_error(ErrorCode::PROTOCOL_ERROR, "frame payload checksum mismatch");
    }
    frame.payload = std::move(payload.value());
  } else if (info.value().payload_crc != crc32(std::span<const std::uint8_t>{})) {
    return transport_error(ErrorCode::PROTOCOL_ERROR, "frame payload checksum mismatch");
  }
  return frame;
}

TcpListener::~TcpListener() { close(); }

Result<std::unique_ptr<TcpListener>> TcpListener::bind(const std::string& address, std::uint16_t port) {
  const Status ready = SocketRuntime::ensure_initialised();
  if (!ready.ok()) {
    return ready;
  }
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  hints.ai_flags = AI_PASSIVE;
  const std::string service = std::to_string(port);
  addrinfo* results = nullptr;
  if (::getaddrinfo(address.empty() ? nullptr : address.c_str(), service.c_str(), &hints, &results) != 0) {
    return transport_error(ErrorCode::INVALID_ARGUMENT, "cannot resolve the requested bind address");
  }
  NativeSocket socket = kInvalidNative;
  for (addrinfo* candidate = results; candidate != nullptr; candidate = candidate->ai_next) {
    socket = ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
    if (socket == kInvalidNative) {
      continue;
    }
    const int reuse = 1;
    ::setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    if (::bind(socket, candidate->ai_addr, static_cast<int>(candidate->ai_addrlen)) == 0 &&
        ::listen(socket, 64) == 0) {
      break;
    }
    close_native(socket);
    socket = kInvalidNative;
  }
  ::freeaddrinfo(results);
  if (socket == kInvalidNative) {
    return transport_error(ErrorCode::TRANSPORT_FAILURE, "cannot bind the coordinator control plane");
  }

  sockaddr_storage local{};
  int local_length = static_cast<int>(sizeof(local));
  std::uint16_t bound_port = port;
  if (::getsockname(socket, reinterpret_cast<sockaddr*>(&local), &local_length) == 0) {
    if (local.ss_family == AF_INET) {
      const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(&local);
      bound_port = ntohs(ipv4->sin_port);
    }
  }

  std::unique_ptr<TcpListener> listener(new TcpListener());
  listener->socket_ = static_cast<std::uintptr_t>(socket);
  listener->port_ = bound_port;
  listener->address_ = address;
  return listener;
}

Result<std::unique_ptr<TcpConnection>> TcpListener::accept_one() {
  if (socket_ == kInvalidSocket) {
    return transport_error(ErrorCode::NOT_FOUND, "listener is closed");
  }
  sockaddr_storage peer{};
  int peer_length = static_cast<int>(sizeof(peer));
  const NativeSocket accepted =
      ::accept(static_cast<NativeSocket>(socket_), reinterpret_cast<sockaddr*>(&peer), &peer_length);
  if (accepted == kInvalidNative) {
    return transport_error(ErrorCode::NOT_FOUND, "listener was closed while waiting for a connection");
  }
  std::string peer_text = "unknown";
  char host[NI_MAXHOST] = {};
  char service[NI_MAXSERV] = {};
  if (::getnameinfo(reinterpret_cast<sockaddr*>(&peer), static_cast<socklen_t>(peer_length), host, sizeof(host),
                    service, sizeof(service), NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
    peer_text = std::string(host) + ":" + std::string(service);
  }
  return TcpConnection::adopt(static_cast<std::uintptr_t>(accepted), std::move(peer_text));
}

void TcpListener::close() {
  if (socket_ == kInvalidSocket) {
    return;
  }
  const NativeSocket socket = static_cast<NativeSocket>(socket_);
  socket_ = kInvalidSocket;
  close_native(socket);
}

}  // namespace experiment_fabric
