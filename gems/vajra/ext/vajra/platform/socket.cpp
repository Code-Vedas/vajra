// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "platform/socket.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

namespace
{
#ifdef _WIN32
  HMODULE winsock_module()
  {
    static HMODULE module = []()
    {
      HMODULE loaded = GetModuleHandleW(L"Ws2_32.dll");
      if (loaded == nullptr)
      {
        loaded = LoadLibraryW(L"Ws2_32.dll");
      }
      if (loaded == nullptr)
      {
        throw std::runtime_error("failed to load Ws2_32.dll");
      }
      return loaded;
    }();
    return module;
  }

  template <typename Function>
  Function winsock_function(const char *name)
  {
    const FARPROC procedure = GetProcAddress(winsock_module(), name);
    if (procedure == nullptr)
    {
      throw std::runtime_error(std::string("failed to resolve Winsock function: ") + name);
    }
    static_assert(sizeof(Function) == sizeof(procedure));
    Function function = nullptr;
    std::memcpy(&function, &procedure, sizeof(function));
    return function;
  }

  using AcceptFunction = SOCKET(WSAAPI *)(SOCKET, sockaddr *, int *);
  using BindFunction = int(WSAAPI *)(SOCKET, const sockaddr *, int);
  using ConnectFunction = int(WSAAPI *)(SOCKET, const sockaddr *, int);
  using ListenFunction = int(WSAAPI *)(SOCKET, int);
  using NameFunction = int(WSAAPI *)(SOCKET, sockaddr *, int *);
  using SocketOptionFunction = int(WSAAPI *)(SOCKET, int, int, const char *, int);
  using GetSocketOptionFunction = int(WSAAPI *)(SOCKET, int, int, char *, int *);
  using IoctlFunction = int(WSAAPI *)(SOCKET, long, u_long *);
  using CloseSocketFunction = int(WSAAPI *)(SOCKET);
  using ShutdownFunction = int(WSAAPI *)(SOCKET, int);
  using ReceiveFunction = int(WSAAPI *)(SOCKET, char *, int, int);
  using SendFunction = int(WSAAPI *)(SOCKET, const char *, int, int);

  AcceptFunction native_accept()
  {
    static const auto value = winsock_function<AcceptFunction>("accept");
    return value;
  }
  BindFunction native_bind()
  {
    static const auto value = winsock_function<BindFunction>("bind");
    return value;
  }
  ConnectFunction native_connect()
  {
    static const auto value = winsock_function<ConnectFunction>("connect");
    return value;
  }
  ListenFunction native_listen()
  {
    static const auto value = winsock_function<ListenFunction>("listen");
    return value;
  }
  NameFunction native_getsockname()
  {
    static const auto value = winsock_function<NameFunction>("getsockname");
    return value;
  }
  NameFunction native_getpeername()
  {
    static const auto value = winsock_function<NameFunction>("getpeername");
    return value;
  }
  SocketOptionFunction native_setsockopt()
  {
    static const auto value = winsock_function<SocketOptionFunction>("setsockopt");
    return value;
  }
  GetSocketOptionFunction native_getsockopt()
  {
    static const auto value = winsock_function<GetSocketOptionFunction>("getsockopt");
    return value;
  }
  IoctlFunction native_ioctlsocket()
  {
    static const auto value = winsock_function<IoctlFunction>("ioctlsocket");
    return value;
  }
  CloseSocketFunction native_closesocket()
  {
    static const auto value = winsock_function<CloseSocketFunction>("closesocket");
    return value;
  }
  ShutdownFunction native_shutdown()
  {
    static const auto value = winsock_function<ShutdownFunction>("shutdown");
    return value;
  }
  ReceiveFunction native_recv()
  {
    static const auto value = winsock_function<ReceiveFunction>("recv");
    return value;
  }
  SendFunction native_send()
  {
    static const auto value = winsock_function<SendFunction>("send");
    return value;
  }

  int windows_socket_error_to_errno(int error_number)
  {
    switch (error_number)
    {
    case WSAEINTR:
      return EINTR;
    case WSAEWOULDBLOCK:
      return EWOULDBLOCK;
    case WSAECONNABORTED:
      return ECONNABORTED;
    case WSAECONNRESET:
      return ECONNRESET;
    case WSAENOTCONN:
      return ENOTCONN;
    case WSAETIMEDOUT:
      return ETIMEDOUT;
    case WSAEADDRINUSE:
      return EADDRINUSE;
    case WSAEACCES:
      return EACCES;
    case WSAEINVAL:
      return EINVAL;
    default:
      return EIO;
    }
  }

  std::string windows_error_message(int error_number)
  {
    char *message = nullptr;
    const DWORD length = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        static_cast<DWORD>(error_number),
        0,
        reinterpret_cast<char *>(&message),
        0,
        nullptr);
    if (length == 0 || message == nullptr)
    {
      return "Winsock error " + std::to_string(error_number);
    }
    std::string result(message, length);
    LocalFree(message);
    while (!result.empty() && (result.back() == '\r' || result.back() == '\n'))
    {
      result.pop_back();
    }
    return result;
  }
#endif
}

Vajra::platform::SocketRuntime::SocketRuntime()
{
#ifdef _WIN32
  WSADATA data{};
  const int status = WSAStartup(MAKEWORD(2, 2), &data);
  if (status != 0)
  {
    throw std::runtime_error("Winsock initialization failed: " + socket_error_message(status));
  }
#endif
}

Vajra::platform::SocketRuntime::~SocketRuntime()
{
#ifdef _WIN32
  WSACleanup();
#endif
}

void Vajra::platform::ensure_socket_runtime()
{
  static const SocketRuntime runtime;
  (void)runtime;
}

bool Vajra::platform::socket_valid(SocketHandle socket)
{
  return socket != kInvalidSocket;
}

bool Vajra::platform::socket_open(SocketHandle socket)
{
  if (!socket_valid(socket))
  {
    return false;
  }
  int socket_type = 0;
#ifdef _WIN32
  int length = sizeof(socket_type);
  return native_getsockopt()(socket, SOL_SOCKET, SO_TYPE, reinterpret_cast<char *>(&socket_type), &length) == 0;
#else
  socklen_t length = sizeof(socket_type);
  return getsockopt(socket, SOL_SOCKET, SO_TYPE, &socket_type, &length) == 0;
#endif
}

Vajra::platform::NativeSocketHandle Vajra::platform::native_socket_handle(SocketHandle socket)
{
  return socket;
}

int Vajra::platform::openssl_socket_descriptor(SocketHandle socket)
{
  if (!socket_valid(socket))
  {
    throw std::invalid_argument("cannot attach an invalid socket to OpenSSL");
  }
  return static_cast<int>(socket);
}

std::uint64_t Vajra::platform::socket_handle_value(SocketHandle socket)
{
  return static_cast<std::uint64_t>(socket);
}

int Vajra::platform::socket_last_error()
{
#ifdef _WIN32
  return WSAGetLastError();
#else
  return errno;
#endif
}

std::string Vajra::platform::socket_error_message(int error_number)
{
#ifdef _WIN32
  return windows_error_message(error_number);
#else
  return std::strerror(error_number);
#endif
}

bool Vajra::platform::socket_error_interrupted(int error_number)
{
#ifdef _WIN32
  return error_number == WSAEINTR || error_number == EINTR;
#else
  return error_number == EINTR;
#endif
}

bool Vajra::platform::socket_error_would_block(int error_number)
{
#ifdef _WIN32
  return error_number == WSAEWOULDBLOCK || error_number == EAGAIN || error_number == EWOULDBLOCK;
#else
  return error_number == EAGAIN || error_number == EWOULDBLOCK;
#endif
}

bool Vajra::platform::socket_error_disconnected(int error_number)
{
#ifdef _WIN32
  return error_number == WSAECONNRESET || error_number == WSAECONNABORTED ||
         error_number == WSAENOTCONN || error_number == WSAESHUTDOWN ||
         error_number == ECONNRESET || error_number == ECONNABORTED ||
         error_number == ENOTCONN || error_number == EPIPE;
#else
  return error_number == ECONNRESET || error_number == ECONNABORTED || error_number == ENOTCONN || error_number == EPIPE;
#endif
}

void Vajra::platform::close_socket(SocketHandle socket)
{
  if (!socket_valid(socket))
  {
    return;
  }
#ifdef _WIN32
  native_closesocket()(socket);
#else
  close(socket);
#endif
}

void Vajra::platform::shutdown_socket(SocketHandle socket)
{
  if (!socket_valid(socket))
  {
    return;
  }
#ifdef _WIN32
  native_shutdown()(socket, SD_BOTH);
#else
  shutdown(socket, SHUT_RDWR);
#endif
}

bool Vajra::platform::shutdown_socket_write(SocketHandle socket)
{
  if (!socket_valid(socket))
  {
    return false;
  }
#ifdef _WIN32
  const int result = native_shutdown()(socket, SD_SEND);
  if (result == SOCKET_ERROR)
  {
    errno = windows_socket_error_to_errno(WSAGetLastError());
    return false;
  }
#else
  const int result = shutdown(socket, SHUT_WR);
#endif
  return result == 0;
}

bool Vajra::platform::set_socket_inheritable(SocketHandle socket, bool inheritable)
{
#ifdef _WIN32
  return SetHandleInformation(
             reinterpret_cast<HANDLE>(socket),
             HANDLE_FLAG_INHERIT,
             inheritable ? HANDLE_FLAG_INHERIT : 0) != 0;
#else
  const int flags = fcntl(socket, F_GETFD);
  if (flags < 0)
  {
    return false;
  }
  const int updated = inheritable ? flags & ~FD_CLOEXEC : flags | FD_CLOEXEC;
  return fcntl(socket, F_SETFD, updated) == 0;
#endif
}

Vajra::platform::SocketHandle Vajra::platform::create_tcp_socket(int family, int type, int protocol)
{
#ifdef _WIN32
  return WSASocketW(family, type, protocol, nullptr, 0, WSA_FLAG_OVERLAPPED);
#else
  return ::socket(family, type, protocol);
#endif
}

bool Vajra::platform::set_socket_option(
    SocketHandle socket,
    int level,
    int option,
    const void *value,
    socklen_t length)
{
#ifdef _WIN32
  return native_setsockopt()(socket, level, option, static_cast<const char *>(value), length) == 0;
#else
  return setsockopt(socket, level, option, value, length) == 0;
#endif
}

bool Vajra::platform::bind_socket(SocketHandle socket, const sockaddr *address, socklen_t address_length)
{
#ifdef _WIN32
  return native_bind()(socket, address, address_length) == 0;
#else
  return bind(socket, address, address_length) == 0;
#endif
}

bool Vajra::platform::connect_socket(SocketHandle socket, const sockaddr *address, socklen_t address_length)
{
#ifdef _WIN32
  return native_connect()(socket, address, static_cast<int>(address_length)) == 0;
#else
  return connect(socket, address, address_length) == 0;
#endif
}

bool Vajra::platform::listen_socket(SocketHandle socket, int backlog)
{
#ifdef _WIN32
  return native_listen()(socket, backlog) == 0;
#else
  return listen(socket, backlog) == 0;
#endif
}

bool Vajra::platform::socket_name(SocketHandle socket, sockaddr *address, socklen_t *address_length)
{
#ifdef _WIN32
  return native_getsockname()(socket, address, address_length) == 0;
#else
  return getsockname(socket, address, address_length) == 0;
#endif
}

bool Vajra::platform::peer_name(SocketHandle socket, sockaddr *address, socklen_t *address_length)
{
#ifdef _WIN32
  return native_getpeername()(socket, address, address_length) == 0;
#else
  return getpeername(socket, address, address_length) == 0;
#endif
}

bool Vajra::platform::set_socket_nonblocking(SocketHandle socket, bool nonblocking)
{
#ifdef _WIN32
  u_long mode = nonblocking ? 1UL : 0UL;
  return native_ioctlsocket()(socket, FIONBIO, &mode) == 0;
#else
  const int flags = fcntl(socket, F_GETFL);
  if (flags < 0)
  {
    return false;
  }
  const int updated = nonblocking ? flags | O_NONBLOCK : flags & ~O_NONBLOCK;
  return fcntl(socket, F_SETFL, updated) == 0;
#endif
}

Vajra::platform::SocketHandle Vajra::platform::accept_socket(
    SocketHandle listener,
    sockaddr *address,
    socklen_t *address_length)
{
#ifdef _WIN32
  return native_accept()(listener, address, address_length);
#else
  return static_cast<SocketHandle>(accept(listener, address, address_length));
#endif
}

bool Vajra::platform::wait_socket(SocketHandle socket, WaitEvent event, int timeout_milliseconds)
{
#ifdef _WIN32
  WSAPOLLFD descriptor{};
  descriptor.fd = socket;
  descriptor.events = event == WaitEvent::read ? POLLRDNORM : POLLWRNORM;
  const short ready_events = descriptor.events | POLLHUP | POLLERR | POLLNVAL;
  for (;;)
  {
    const int result = WSAPoll(&descriptor, 1, timeout_milliseconds);
    if (result > 0)
    {
      return (descriptor.revents & ready_events) != 0;
    }
    if (result == 0 || !socket_error_interrupted(socket_last_error()))
    {
      return false;
    }
  }
#else
  pollfd descriptor{};
  descriptor.fd = socket;
  descriptor.events = event == WaitEvent::read ? POLLIN : POLLOUT;
  const short ready_events = descriptor.events | POLLHUP | POLLERR | POLLNVAL;
  for (;;)
  {
    const int result = poll(&descriptor, 1, timeout_milliseconds);
    if (result > 0)
    {
      return (descriptor.revents & ready_events) != 0;
    }
    if (result == 0 || !socket_error_interrupted(socket_last_error()))
    {
      return false;
    }
  }
#endif
}

Vajra::platform::SignedSize Vajra::platform::receive_socket(SocketHandle socket, char *buffer, std::size_t length)
{
#ifdef _WIN32
  const int chunk = static_cast<int>(std::min(length, static_cast<std::size_t>(std::numeric_limits<int>::max())));
  const int result = native_recv()(socket, buffer, chunk, 0);
  if (result == SOCKET_ERROR)
  {
    errno = windows_socket_error_to_errno(WSAGetLastError());
  }
  return result;
#else
  return recv(socket, buffer, length, 0);
#endif
}

Vajra::platform::SignedSize Vajra::platform::peek_socket(SocketHandle socket, char *buffer, std::size_t length)
{
#ifdef _WIN32
  const int chunk = static_cast<int>(std::min(length, static_cast<std::size_t>(std::numeric_limits<int>::max())));
  const int result = native_recv()(socket, buffer, chunk, MSG_PEEK);
  if (result == SOCKET_ERROR)
  {
    errno = windows_socket_error_to_errno(WSAGetLastError());
  }
  return result;
#else
  return recv(socket, buffer, length, MSG_PEEK);
#endif
}

Vajra::platform::SignedSize Vajra::platform::send_socket(SocketHandle socket, const char *buffer, std::size_t length)
{
#ifdef _WIN32
  const int chunk = static_cast<int>(std::min(length, static_cast<std::size_t>(std::numeric_limits<int>::max())));
  const int result = native_send()(socket, buffer, chunk, 0);
  if (result == SOCKET_ERROR)
  {
    errno = windows_socket_error_to_errno(WSAGetLastError());
  }
  return result;
#elif defined(MSG_NOSIGNAL)
  return send(socket, buffer, length, MSG_NOSIGNAL);
#else
  return send(socket, buffer, length, 0);
#endif
}
