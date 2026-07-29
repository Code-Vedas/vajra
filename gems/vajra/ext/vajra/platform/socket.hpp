// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_PLATFORM_SOCKET_HPP
#define VAJRA_PLATFORM_SOCKET_HPP

#include <cstddef>
#include <cstdint>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#endif

namespace Vajra::platform
{
#ifdef _WIN32
  using SocketHandle = SOCKET;
  using NativeSocketHandle = SOCKET;
  using SignedSize = std::intptr_t;
  constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
  using SocketHandle = int;
  using NativeSocketHandle = int;
  using SignedSize = ssize_t;
  constexpr SocketHandle kInvalidSocket = -1;
#endif

  enum class WaitEvent : std::uint8_t
  {
    read,
    write,
  };

  class SocketRuntime final
  {
  public:
    SocketRuntime();
    ~SocketRuntime();

    SocketRuntime(const SocketRuntime &) = delete;
    SocketRuntime &operator=(const SocketRuntime &) = delete;
  };

  void ensure_socket_runtime();
  bool socket_valid(SocketHandle socket);
  bool socket_open(SocketHandle socket);
  NativeSocketHandle native_socket_handle(SocketHandle socket);
  int openssl_socket_descriptor(SocketHandle socket);
  std::uint64_t socket_handle_value(SocketHandle socket);
  int socket_last_error();
  std::string socket_error_message(int error_number);
  bool socket_error_interrupted(int error_number);
  bool socket_error_would_block(int error_number);
  bool socket_error_disconnected(int error_number);
  void close_socket(SocketHandle socket);
  void shutdown_socket(SocketHandle socket);
  bool shutdown_socket_write(SocketHandle socket);
  bool set_socket_inheritable(SocketHandle socket, bool inheritable);
  bool set_socket_nonblocking(SocketHandle socket, bool nonblocking);
  SocketHandle create_tcp_socket(int family, int type, int protocol);
  bool set_socket_option(SocketHandle socket, int level, int option, const void *value, socklen_t length);
  bool bind_socket(SocketHandle socket, const sockaddr *address, socklen_t address_length);
  bool connect_socket(SocketHandle socket, const sockaddr *address, socklen_t address_length);
  bool listen_socket(SocketHandle socket, int backlog);
  bool socket_name(SocketHandle socket, sockaddr *address, socklen_t *address_length);
  bool peer_name(SocketHandle socket, sockaddr *address, socklen_t *address_length);
  SocketHandle accept_socket(SocketHandle listener, sockaddr *address, socklen_t *address_length);
  bool wait_socket(SocketHandle socket, WaitEvent event, int timeout_milliseconds);
  SignedSize receive_socket(SocketHandle socket, char *buffer, std::size_t length);
  SignedSize peek_socket(SocketHandle socket, char *buffer, std::size_t length);
  SignedSize send_socket(SocketHandle socket, const char *buffer, std::size_t length);
}

#endif
