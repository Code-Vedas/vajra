// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "runtime/native_runtime.hpp"

#include "listener/listener_socket.hpp"
#include "rack/rack_request_executor.hpp"
#include "rack/ruby_rack_transport.hpp"
#include "request/http2_session.hpp"
#include "request/request_processor.hpp"
#include "response/response_writer.hpp"
#include "ruby/thread.h"
#include "ruby/version.h"
#include "runtime/boot_contract.hpp"
#include "runtime/runtime_logging.hpp"
#include "transport/connection.hpp"
#include "transport/tls_connection.hpp"
#include "vajra.hpp"

#include <arpa/inet.h>
#include <algorithm>
#include <atomic>
#include <array>
#include <cerrno>
#include <cstdint>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sstream>
#include <stdexcept>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unordered_map>
#include <utility>
#include <unistd.h>

#if defined(__linux__) && __has_include(<malloc.h>)
#include <malloc.h>
#endif

#if defined(__APPLE__)
#include <sys/event.h>
#else
#include <sys/epoll.h>
#endif

namespace
{
  volatile std::sig_atomic_t shutting_down = 0;
  constexpr const char *kMasterPreloadRuntimeRole = "ruby_master_preload";
  constexpr const char *kWorkerBootstrapRuntimeRole = "ruby_worker_bootstrap";
  constexpr std::size_t kMaxWorkerBootstrapStringPayloadBytes = 64 * 1024;
  constexpr auto kWorkerTimeoutGracePeriod = std::chrono::seconds(1);
  constexpr auto kWorkerExitWatcherIdlePollInterval = std::chrono::seconds(1);
  constexpr int kSignalRetryLimit = 5;
  constexpr std::uint64_t kReplacementFailureLimit = 3;
  constexpr const char *kHttp2ClientPreface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
  constexpr std::size_t kHttp2ClientPrefaceLength = 24;
#if defined(__linux__) && __has_include(<malloc.h>)
  std::atomic<std::int64_t> last_malloc_trim_milliseconds{0};
#endif

  void block_ruby_reserved_signals_for_native_thread()
  {
#ifdef SIGBUS
    sigset_t blocked_signals;
    sigemptyset(&blocked_signals);
    sigaddset(&blocked_signals, SIGBUS);
    pthread_sigmask(SIG_BLOCK, &blocked_signals, nullptr);
#endif
  }

  void maybe_trim_linux_malloc()
  {
#if defined(__linux__) && __has_include(<malloc.h>)
    const auto now = std::chrono::steady_clock::now();
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    std::int64_t previous = last_malloc_trim_milliseconds.load(std::memory_order_acquire);
    while (milliseconds - previous >= 250)
    {
      if (last_malloc_trim_milliseconds.compare_exchange_weak(
              previous,
              milliseconds,
              std::memory_order_acq_rel,
              std::memory_order_acquire))
      {
        (void)malloc_trim(0);
        return;
      }
    }
#endif
  }

  enum class WorkerBootstrapStatus : std::uint8_t
  {
    ready = 1,
    failed = 2,
  };

  enum class WorkerSpawnerCommand : std::uint8_t
  {
    spawn = 1,
    shutdown = 2,
  };

  struct WorkerBootstrapReport
  {
    WorkerBootstrapStatus status;
    std::optional<Vajra::runtime::BootDiagnostic> diagnostic;
  };

  struct WorkerSpawnerResponseHeader
  {
    WorkerBootstrapStatus status;
    std::int32_t pid;
    std::uint32_t control_channel_count;
  };

  struct WorkerWaitContext
  {
    Vajra::runtime::NativeRuntime *runtime;
    const std::vector<std::shared_ptr<Vajra::runtime::SharedWorkerState>> *worker_states;
    std::string error_message;
  };

  struct RuntimeSleepContext
  {
    std::chrono::milliseconds duration{50};
  };

  VALUE notify_tracing_after_fork_protected(VALUE)
  {
    const ID id_after_fork = rb_intern("after_fork!");
    const VALUE vajra = rb_const_get(rb_cObject, rb_intern("Vajra"));
    const VALUE internal = rb_const_get(vajra, rb_intern("Internal"));
    const VALUE tracing = rb_const_get(internal, rb_intern("Tracing"));
    if (rb_respond_to(tracing, id_after_fork))
    {
      rb_funcall(tracing, id_after_fork, 0);
    }
    return Qnil;
  }

  void notify_tracing_after_fork()
  {
    int state = 0;
    rb_protect(notify_tracing_after_fork_protected, Qnil, &state);
    if (state != 0)
    {
      rb_set_errinfo(Qnil);
    }
  }

  void handle_signal(int sig)
  {
    if (sig == SIGINT || sig == SIGTERM)
    {
      shutting_down = 1;
    }
  }

  class SignalHandlerGuard
  {
  public:
    SignalHandlerGuard()
    {
      std::memset(&new_action_, 0, sizeof(new_action_));
      std::memset(&previous_int_action_, 0, sizeof(previous_int_action_));
      std::memset(&previous_term_action_, 0, sizeof(previous_term_action_));
      new_action_.sa_handler = handle_signal;
      sigemptyset(&new_action_.sa_mask);
    }

    void install()
    {
      if (sigaction(SIGINT, &new_action_, &previous_int_action_) != 0)
      {
        throw std::runtime_error("failed to install SIGINT handler");
      }
      if (sigaction(SIGTERM, &new_action_, &previous_term_action_) != 0)
      {
        sigaction(SIGINT, &previous_int_action_, nullptr);
        throw std::runtime_error("failed to install SIGTERM handler");
      }
      installed_ = true;
    }

    ~SignalHandlerGuard()
    {
      if (!installed_)
      {
        return;
      }

      sigaction(SIGINT, &previous_int_action_, nullptr);
      sigaction(SIGTERM, &previous_term_action_, nullptr);
    }

  private:
    struct sigaction new_action_;
    struct sigaction previous_int_action_;
    struct sigaction previous_term_action_;
    bool installed_ = false;
  };

  bool start_called_from_ruby_main_thread()
  {
    return rb_equal(rb_thread_current(), rb_thread_main()) == Qtrue;
  }

  void close_fd_if_open(int fd)
  {
    if (fd >= 0)
    {
      close(fd);
    }
  }

#if !defined(__linux__) || !defined(SOCK_CLOEXEC)
  int set_fd_cloexec(int fd)
  {
    const int existing_flags = fcntl(fd, F_GETFD);
    if (existing_flags < 0)
    {
      return -1;
    }

    return fcntl(fd, F_SETFD, existing_flags | FD_CLOEXEC);
  }
#endif

  int accept_client_cloexec(int listener_fd, sockaddr *client_addr, socklen_t *client_len)
  {
#if defined(__linux__) && defined(SOCK_CLOEXEC)
    return accept4(listener_fd, client_addr, client_len, SOCK_CLOEXEC);
#else
    const int client_fd = accept(listener_fd, client_addr, client_len);
    if (client_fd < 0)
    {
      return client_fd;
    }

    if (set_fd_cloexec(client_fd) != 0)
    {
      const int error_number = errno;
      close(client_fd);
      errno = error_number;
      return -1;
    }

    return client_fd;
#endif
  }

  int worker_control_socket_type()
  {
#if defined(__APPLE__)
    return SOCK_STREAM;
#else
    return SOCK_SEQPACKET;
#endif
  }

  std::size_t checked_multiply(std::size_t left, std::size_t right, const char *error_message)
  {
    if (left != 0 && right > (std::numeric_limits<std::size_t>::max() / left))
    {
      throw std::runtime_error(error_message);
    }

    return left * right;
  }

  std::size_t checked_add(std::size_t left, std::size_t right, const char *error_message)
  {
    if (right > (std::numeric_limits<std::size_t>::max() - left))
    {
      throw std::runtime_error(error_message);
    }

    return left + right;
  }

  void validate_worker_channel_capacity(int workers)
  {
    constexpr std::size_t kRuntimeFdReserve = 32;
    const std::size_t total_control_channels = checked_multiply(
        static_cast<std::size_t>(workers),
        static_cast<std::size_t>(2),
        "invalid workers combination: worker control channel fd count is too large");
    const std::size_t boot_readiness_pipe_fds = static_cast<std::size_t>(2);
    const std::size_t boot_overhead_fds = checked_add(
        static_cast<std::size_t>(2),
        boot_readiness_pipe_fds,
        "invalid workers/threads combination: worker boot fd count is too large");
    const std::size_t peak_parent_fds = checked_add(
        total_control_channels,
        boot_overhead_fds,
        "invalid workers/threads combination: required fd count is too large");
    const std::size_t required_fds = checked_add(
        peak_parent_fds,
        kRuntimeFdReserve,
        "invalid workers/threads combination: required fd count is too large");

    rlimit fd_limit{};
    if (getrlimit(RLIMIT_NOFILE, &fd_limit) != 0 || fd_limit.rlim_cur == RLIM_INFINITY)
    {
      return;
    }

    const std::size_t available_fds = static_cast<std::size_t>(fd_limit.rlim_cur);
    if (required_fds > available_fds)
    {
      throw std::runtime_error(
          "invalid workers combination: workers would keep " +
          std::to_string(total_control_channels) + " parent control-channel fds open in steady state and peak at " +
          std::to_string(peak_parent_fds) + " parent fds during worker boot (" +
          "2 boot control-channel fds plus " +
          std::to_string(boot_readiness_pipe_fds) + " readiness-pipe fds), which exceeds the process fd limit of " +
          std::to_string(available_fds) + ". Lower workers, or raise the fd limit.");
    }
  }

  bool shutdown_requested_or_runtime_draining()
  {
    return Vajra::runtime::NativeRuntime::shutdown_requested() ||
           (Vajra::runtime::current_runtime_state() != nullptr &&
            Vajra::runtime::current_runtime_state()->shutdown_requested.load(std::memory_order_acquire));
  }

  std::string socket_address(sockaddr_in address)
  {
    char buffer[INET_ADDRSTRLEN] = {0};
    if (inet_ntop(AF_INET, &address.sin_addr, buffer, sizeof(buffer)) == nullptr)
    {
      return "0.0.0.0";
    }

    return buffer;
  }

  Vajra::request::SocketContext socket_context_for_client_fd(int client_fd, int fallback_port)
  {
    sockaddr_in remote_addr{};
    socklen_t remote_addr_length = sizeof(remote_addr);
    if (getpeername(client_fd, reinterpret_cast<sockaddr *>(&remote_addr), &remote_addr_length) != 0)
    {
      remote_addr.sin_family = AF_INET;
      remote_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      remote_addr.sin_port = htons(0);
    }

    sockaddr_in local_addr{};
    socklen_t local_addr_length = sizeof(local_addr);
    if (getsockname(client_fd, reinterpret_cast<sockaddr *>(&local_addr), &local_addr_length) != 0)
    {
      local_addr.sin_family = AF_INET;
      local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
      local_addr.sin_port = htons(static_cast<std::uint16_t>(fallback_port));
    }

    return Vajra::request::SocketContext{
        socket_address(remote_addr),
        ntohs(remote_addr.sin_port),
        socket_address(local_addr),
        ntohs(local_addr.sin_port),
        "http"};
  }

  void *sleep_runtime_loop_without_gvl(void *data)
  {
    auto *context = static_cast<RuntimeSleepContext *>(data);
    std::this_thread::sleep_for(context->duration);
    return nullptr;
  }

  void write_all_or_throw(int fd, const void *buffer, std::size_t length)
  {
    const auto *bytes = static_cast<const std::uint8_t *>(buffer);
    std::size_t written = 0;
    while (written < length)
    {
      const ssize_t result = write(fd, bytes + written, length - written);
      if (result < 0)
      {
        if (errno == EINTR)
        {
          continue;
        }
        throw std::runtime_error("worker bootstrap pipe write failed");
      }

      written += static_cast<std::size_t>(result);
    }
  }

  bool read_exact_or_eof(int fd, void *buffer, std::size_t length)
  {
    auto *bytes = static_cast<std::uint8_t *>(buffer);
    std::size_t read_bytes = 0;
    while (read_bytes < length)
    {
      const ssize_t result = read(fd, bytes + read_bytes, length - read_bytes);
      if (result == 0)
      {
        if (read_bytes == 0)
        {
          return false;
        }

        throw std::runtime_error("worker bootstrap pipe closed unexpectedly");
      }

      if (result < 0)
      {
        if (errno == EINTR)
        {
          continue;
        }
        throw std::runtime_error("worker bootstrap pipe read failed");
      }

      read_bytes += static_cast<std::size_t>(result);
    }

    return true;
  }

  void write_string_payload(int fd, const std::string &value)
  {
    const std::string payload = value.substr(0, kMaxWorkerBootstrapStringPayloadBytes);
    const std::uint32_t length = static_cast<std::uint32_t>(payload.size());
    write_all_or_throw(fd, &length, sizeof(length));
    if (length > 0)
    {
      write_all_or_throw(fd, payload.data(), length);
    }
  }

  std::string read_string_payload(int fd)
  {
    std::uint32_t length = 0;
    if (!read_exact_or_eof(fd, &length, sizeof(length)))
    {
      throw std::runtime_error("worker bootstrap pipe closed before string payload length");
    }
    if (length > kMaxWorkerBootstrapStringPayloadBytes)
    {
      throw std::runtime_error("worker bootstrap pipe string payload exceeds maximum size");
    }
    std::string value(length, '\0');
    if (length > 0)
    {
      if (!read_exact_or_eof(fd, value.data(), length))
      {
        throw std::runtime_error("worker bootstrap pipe closed before string payload body");
      }
    }
    return value;
  }

  void report_worker_boot_ready(int write_fd)
  {
    const auto status = static_cast<std::uint8_t>(WorkerBootstrapStatus::ready);
    write_all_or_throw(write_fd, &status, sizeof(status));
  }

  void report_worker_boot_failed(int write_fd, const Vajra::runtime::BootDiagnostic &diagnostic)
  {
    const auto status = static_cast<std::uint8_t>(WorkerBootstrapStatus::failed);
    write_all_or_throw(write_fd, &status, sizeof(status));
    write_string_payload(write_fd, diagnostic.code);
    write_string_payload(write_fd, diagnostic.category);
    write_string_payload(write_fd, diagnostic.message);
  }

  bool wait_worker_bootstrap_readable(int read_fd, int timeout_seconds)
  {
    pollfd descriptor{read_fd, POLLIN | POLLHUP | POLLERR, 0};
    const int timeout_milliseconds = std::max(1, timeout_seconds) * 1000;
    for (;;)
    {
      const int result = poll(&descriptor, 1, timeout_milliseconds);
      if (result > 0)
      {
        return (descriptor.revents & (POLLIN | POLLHUP | POLLERR)) != 0;
      }
      if (result == 0)
      {
        return false;
      }
      if (errno != EINTR)
      {
        throw std::runtime_error(std::string("worker bootstrap readiness poll failed: ") + std::strerror(errno));
      }
    }
  }

  WorkerBootstrapReport read_worker_bootstrap_report(int read_fd, int timeout_seconds)
  {
    if (!wait_worker_bootstrap_readable(read_fd, timeout_seconds))
    {
      throw std::runtime_error("worker bootstrap readiness timed out");
    }

    std::uint8_t status = 0;
    if (!read_exact_or_eof(read_fd, &status, sizeof(status)))
    {
      throw std::runtime_error("worker exited before reporting readiness");
    }

    if (status == static_cast<std::uint8_t>(WorkerBootstrapStatus::ready))
    {
      return WorkerBootstrapReport{WorkerBootstrapStatus::ready, std::nullopt};
    }

    if (status == static_cast<std::uint8_t>(WorkerBootstrapStatus::failed))
    {
      return WorkerBootstrapReport{
          WorkerBootstrapStatus::failed,
          Vajra::runtime::BootDiagnostic{
              read_string_payload(read_fd),
              read_string_payload(read_fd),
              read_string_payload(read_fd)}};
    }

    throw std::runtime_error("worker reported an unknown bootstrap state");
  }

  void send_file_descriptors_or_throw(int socket_fd, const std::vector<int> &fds)
  {
    if (fds.empty())
    {
      return;
    }

    char message = 'F';
    struct iovec io_vector;
    std::memset(&io_vector, 0, sizeof(io_vector));
    io_vector.iov_base = &message;
    io_vector.iov_len = sizeof(message);
    std::vector<char> control(CMSG_SPACE(sizeof(int) * fds.size()));
    std::memset(control.data(), 0, control.size());

    struct msghdr message_header;
    std::memset(&message_header, 0, sizeof(message_header));
    message_header.msg_iov = &io_vector;
    message_header.msg_iovlen = 1;
    message_header.msg_control = control.data();
    message_header.msg_controllen = control.size();

    struct cmsghdr *control_message = CMSG_FIRSTHDR(&message_header);
    control_message->cmsg_level = SOL_SOCKET;
    control_message->cmsg_type = SCM_RIGHTS;
    control_message->cmsg_len = CMSG_LEN(sizeof(int) * fds.size());
    std::memcpy(CMSG_DATA(control_message), fds.data(), sizeof(int) * fds.size());

    while (sendmsg(socket_fd, &message_header, 0) < 0)
    {
      if (errno == EINTR)
      {
        continue;
      }
      throw std::runtime_error("control socket fd send failed");
    }
  }

  void send_single_file_descriptor_or_throw(int socket_fd, int fd)
  {
    char message = 'F';
    struct iovec io_vector;
    std::memset(&io_vector, 0, sizeof(io_vector));
    io_vector.iov_base = &message;
    io_vector.iov_len = sizeof(message);

    alignas(struct cmsghdr) char control[CMSG_SPACE(sizeof(int))];
    std::memset(control, 0, sizeof(control));

    struct msghdr message_header;
    std::memset(&message_header, 0, sizeof(message_header));
    message_header.msg_iov = &io_vector;
    message_header.msg_iovlen = 1;
    message_header.msg_control = control;
    message_header.msg_controllen = sizeof(control);

    struct cmsghdr *control_message = CMSG_FIRSTHDR(&message_header);
    control_message->cmsg_level = SOL_SOCKET;
    control_message->cmsg_type = SCM_RIGHTS;
    control_message->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(control_message), &fd, sizeof(fd));

    while (sendmsg(socket_fd, &message_header, 0) < 0)
    {
      if (errno == EINTR)
      {
        continue;
      }
      throw std::runtime_error("control socket fd send failed");
    }
  }

  std::vector<int> receive_file_descriptors_or_throw(int socket_fd, std::size_t expected_count)
  {
    if (expected_count == 0)
    {
      return {};
    }

    char message = '\0';
    struct iovec io_vector;
    std::memset(&io_vector, 0, sizeof(io_vector));
    io_vector.iov_base = &message;
    io_vector.iov_len = sizeof(message);
    std::vector<char> control(CMSG_SPACE(sizeof(int) * expected_count));
    std::memset(control.data(), 0, control.size());

    struct msghdr message_header;
    std::memset(&message_header, 0, sizeof(message_header));
    message_header.msg_iov = &io_vector;
    message_header.msg_iovlen = 1;
    message_header.msg_control = control.data();
    message_header.msg_controllen = control.size();

    ssize_t result = -1;
    while ((result = recvmsg(socket_fd, &message_header, 0)) < 0)
    {
      if (errno == EINTR)
      {
        continue;
      }
      throw std::runtime_error("control socket fd receive failed");
    }

    if (result <= 0)
    {
      throw std::runtime_error("control socket closed before sending file descriptors");
    }

    const struct cmsghdr *control_message = CMSG_FIRSTHDR(&message_header);
    if (control_message == nullptr ||
        control_message->cmsg_level != SOL_SOCKET ||
        control_message->cmsg_type != SCM_RIGHTS)
    {
      throw std::runtime_error("control socket returned an invalid file-descriptor payload");
    }

    const std::size_t descriptor_count =
        static_cast<std::size_t>((control_message->cmsg_len - CMSG_LEN(0)) / sizeof(int));
    if (descriptor_count != expected_count)
    {
      throw std::runtime_error("control socket returned an unexpected file-descriptor count");
    }

    std::vector<int> descriptors(descriptor_count, -1);
    std::memcpy(descriptors.data(), CMSG_DATA(control_message), sizeof(int) * descriptor_count);
    return descriptors;
  }

  std::optional<int> receive_single_file_descriptor(int socket_fd)
  {
    char message = '\0';
    struct iovec io_vector;
    std::memset(&io_vector, 0, sizeof(io_vector));
    io_vector.iov_base = &message;
    io_vector.iov_len = sizeof(message);
    std::array<char, CMSG_SPACE(sizeof(int))> control{};

    struct msghdr message_header;
    std::memset(&message_header, 0, sizeof(message_header));
    message_header.msg_iov = &io_vector;
    message_header.msg_iovlen = 1;
    message_header.msg_control = control.data();
    message_header.msg_controllen = control.size();

    for (;;)
    {
      const ssize_t result = recvmsg(socket_fd, &message_header, 0);
      if (result == 0)
      {
        return std::nullopt;
      }
      if (result < 0)
      {
        if (errno == EINTR)
        {
          if (shutdown_requested_or_runtime_draining())
          {
            return std::nullopt;
          }
          continue;
        }
        if (errno == EBADF || errno == ENOTSOCK)
        {
          return std::nullopt;
        }
        throw std::runtime_error(std::string("control socket receive failed: ") + std::strerror(errno));
      }

      struct cmsghdr *control_message = CMSG_FIRSTHDR(&message_header);
      if (control_message == nullptr ||
          control_message->cmsg_level != SOL_SOCKET ||
          control_message->cmsg_type != SCM_RIGHTS ||
          (control_message->cmsg_len - CMSG_LEN(0)) < static_cast<socklen_t>(sizeof(int)))
      {
        throw std::runtime_error("control socket received an invalid file descriptor payload");
      }

      int fd = -1;
      std::memcpy(&fd, CMSG_DATA(control_message), sizeof(fd));
      return fd;
    }
  }

  struct WorkerHotPathLoopContext
  {
    Vajra::request::RequestProcessor *request_processor = nullptr;
    int listener_fd = -1;
    int control_fd = -1;
    int fallback_port = 0;
    int worker_index = -1;
    struct WorkerConnectionQueueState *connection_queue_state = nullptr;
    Vajra::transport::TlsContext *tls_context = nullptr;
    bool http2_enabled = false;
    Vajra::request::Http2Config http2_config;
    std::shared_ptr<const Vajra::request::RequestExecutor> request_executor;
    int request_head_timeout_seconds = 5;
    int first_data_timeout_seconds = 30;
    int request_body_timeout_seconds = Vajra::request::kDefaultRequestBodyTimeoutSeconds;
    int persistent_timeout_seconds = 30;
    std::size_t max_keepalive_requests = 0;
  };

  struct CachedWorkerDispatchTarget
  {
    std::shared_ptr<Vajra::runtime::SharedWorkerState> worker_state;
    std::uint64_t control_channel_generation = 0;
    int control_fd = -1;
  };

  constexpr std::size_t kMaxWarmWorkerConnectionStates = 128;

  struct WorkerConnectionState
  {
    int client_fd = -1;
    std::string buffered_bytes;
    bool first_request = true;
    std::size_t completed_requests = 0;
    bool tls_owned_by_worker = false;
    std::chrono::steady_clock::time_point idle_started_at = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point head_started_at = std::chrono::steady_clock::now();
    bool head_started = false;
    bool head_complete_buffered = false;
  };

  enum class H2cPrefaceProbeOutcome
  {
    not_preface,
    prior_knowledge,
    invalid_preface,
    await_more,
    closed
  };

  bool h2c_preface_candidate(const std::string &buffered_bytes)
  {
    static constexpr std::string_view kPriorKnowledgeMethod = "PRI ";
    return buffered_bytes.size() >= kPriorKnowledgeMethod.size() &&
           std::memcmp(buffered_bytes.data(), kPriorKnowledgeMethod.data(), kPriorKnowledgeMethod.size()) == 0;
  }

  bool h2c_invalid_preface_probe(const std::string &buffered_bytes)
  {
    static constexpr std::string_view kInvalidConnectionPreface = "INVALID CONNECTION PREFACE";
    return buffered_bytes.size() >= kInvalidConnectionPreface.size() &&
           std::memcmp(buffered_bytes.data(), kInvalidConnectionPreface.data(), kInvalidConnectionPreface.size()) == 0;
  }

  bool h2c_preface_prefix_matches(const std::string &buffered_bytes)
  {
    const std::size_t compared_bytes = std::min(buffered_bytes.size(), kHttp2ClientPrefaceLength);
    return std::memcmp(buffered_bytes.data(), kHttp2ClientPreface, compared_bytes) == 0;
  }

  bool write_h2c_bad_request(Vajra::transport::Connection &connection)
  {
    static constexpr const char kBadRequest[] =
        "HTTP/1.1 400 Bad Request\r\n"
        "Connection: close\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 12\r\n"
        "\r\n"
        "Bad Request\n";
    std::size_t written = 0;
    while (written < sizeof(kBadRequest) - 1)
    {
      const ssize_t result = connection.write(kBadRequest + written, sizeof(kBadRequest) - 1 - written);
      if (result <= 0)
      {
        return false;
      }
      written += static_cast<std::size_t>(result);
    }
    return true;
  }

  bool write_h2c_protocol_goaway(Vajra::transport::Connection &connection)
  {
    std::array<std::uint8_t, 17> frame{};
    frame[2] = 8;
    frame[3] = 7;
    frame[16] = 1;

    std::size_t written = 0;
    while (written < frame.size())
    {
      const ssize_t result = connection.write(
          reinterpret_cast<const char *>(frame.data() + written),
          frame.size() - written);
      if (result <= 0)
      {
        return false;
      }
      written += static_cast<std::size_t>(result);
    }
    return true;
  }

  H2cPrefaceProbeOutcome probe_h2c_prior_knowledge(
      Vajra::transport::Connection &connection,
      WorkerConnectionState &connection_state,
      int first_data_timeout_seconds)
  {
    if (!connection_state.first_request)
    {
      return H2cPrefaceProbeOutcome::not_preface;
    }

    if (!connection_state.buffered_bytes.empty() &&
        !h2c_preface_prefix_matches(connection_state.buffered_bytes))
    {
      return h2c_preface_candidate(connection_state.buffered_bytes) ||
                     h2c_invalid_preface_probe(connection_state.buffered_bytes)
                 ? H2cPrefaceProbeOutcome::invalid_preface
                 : H2cPrefaceProbeOutcome::not_preface;
    }

    while (connection_state.buffered_bytes.size() < kHttp2ClientPrefaceLength)
    {
      if (!connection.wait_readable(first_data_timeout_seconds))
      {
        return connection_state.buffered_bytes.empty()
                   ? H2cPrefaceProbeOutcome::not_preface
                   : H2cPrefaceProbeOutcome::await_more;
      }

      char buffer[kHttp2ClientPrefaceLength];
      const std::size_t remaining = kHttp2ClientPrefaceLength - connection_state.buffered_bytes.size();
      const ssize_t read_bytes = connection.read(buffer, remaining);
      if (read_bytes == 0)
      {
        return H2cPrefaceProbeOutcome::closed;
      }
      if (read_bytes < 0)
      {
        throw std::runtime_error("h2c preface read failed");
      }
      connection_state.buffered_bytes.append(buffer, static_cast<std::size_t>(read_bytes));
      if (!h2c_preface_prefix_matches(connection_state.buffered_bytes))
      {
        return h2c_preface_candidate(connection_state.buffered_bytes) ||
                       h2c_invalid_preface_probe(connection_state.buffered_bytes)
                   ? H2cPrefaceProbeOutcome::invalid_preface
                   : H2cPrefaceProbeOutcome::not_preface;
      }
    }

    return H2cPrefaceProbeOutcome::prior_knowledge;
  }

  struct WorkerConnectionQueueState
  {
    std::mutex mutex;
    std::condition_variable condition;
    std::deque<std::shared_ptr<WorkerConnectionState>> pending_connections;
    std::unordered_map<int, std::shared_ptr<WorkerConnectionState>> idle_connections;
    std::unordered_map<int, std::shared_ptr<WorkerConnectionState>> active_connections;
    std::vector<std::shared_ptr<WorkerConnectionState>> connection_pool;
    std::size_t max_pending_connections = 0;
    std::size_t max_active_connections = 0;
    int reactor_fd = -1;
    bool capacity_gated = false;
    bool shutdown_requested = false;
  };

#if defined(__APPLE__)
  int create_worker_reactor_fd()
  {
    return kqueue();
  }

  bool add_worker_reactor_fd(int reactor_fd, int client_fd)
  {
    struct kevent event;
    EV_SET(&event, client_fd, EVFILT_READ, EV_ADD | EV_ENABLE | EV_CLEAR, 0, 0, nullptr);
    return kevent(reactor_fd, &event, 1, nullptr, 0, nullptr) == 0;
  }

  void remove_worker_reactor_fd(int reactor_fd, int client_fd)
  {
    if (reactor_fd < 0 || client_fd < 0)
    {
      return;
    }
    struct kevent event;
    EV_SET(&event, client_fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    (void)kevent(reactor_fd, &event, 1, nullptr, 0, nullptr);
  }

  void wait_worker_reactor_events(int reactor_fd, int timeout_milliseconds, std::vector<int> &ready_fds)
  {
    ready_fds.clear();
    std::array<struct kevent, 128> events;
    struct timespec timeout;
    timeout.tv_sec = timeout_milliseconds / 1000;
    timeout.tv_nsec = (timeout_milliseconds % 1000) * 1000000L;
    const int ready = kevent(reactor_fd, nullptr, 0, events.data(), static_cast<int>(events.size()), &timeout);
    if (ready < 0)
    {
      if (errno == EINTR)
      {
        return;
      }
      throw std::runtime_error(std::string("worker reactor wait failed: ") + std::strerror(errno));
    }

    ready_fds.reserve(static_cast<std::size_t>(ready));
    for (int index = 0; index < ready; ++index)
    {
      ready_fds.push_back(static_cast<int>(events[static_cast<std::size_t>(index)].ident));
    }
  }
#else
  int create_worker_reactor_fd()
  {
    return epoll_create1(EPOLL_CLOEXEC);
  }

  bool add_worker_reactor_fd(int reactor_fd, int client_fd)
  {
    epoll_event event;
    std::memset(&event, 0, sizeof(event));
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    event.data.fd = client_fd;
    return epoll_ctl(reactor_fd, EPOLL_CTL_ADD, client_fd, &event) == 0;
  }

  void remove_worker_reactor_fd(int reactor_fd, int client_fd)
  {
    if (reactor_fd < 0 || client_fd < 0)
    {
      return;
    }
    (void)epoll_ctl(reactor_fd, EPOLL_CTL_DEL, client_fd, nullptr);
  }

  void wait_worker_reactor_events(int reactor_fd, int timeout_milliseconds, std::vector<int> &ready_fds)
  {
    ready_fds.clear();
    std::array<epoll_event, 128> events;
    const int ready = epoll_wait(reactor_fd, events.data(), static_cast<int>(events.size()), timeout_milliseconds);
    if (ready < 0)
    {
      if (errno == EINTR)
      {
        return;
      }
      throw std::runtime_error(std::string("worker reactor wait failed: ") + std::strerror(errno));
    }

    ready_fds.reserve(static_cast<std::size_t>(ready));
    for (int index = 0; index < ready; ++index)
    {
      ready_fds.push_back(events[static_cast<std::size_t>(index)].data.fd);
    }
  }
#endif

  void close_worker_connection_state(WorkerConnectionState &connection)
  {
    close_fd_if_open(connection.client_fd);
    connection.client_fd = -1;
  }

  void close_worker_connection_state_and_note(WorkerConnectionState &connection)
  {
    if (connection.client_fd >= 0)
    {
      close_worker_connection_state(connection);
      Vajra::runtime::note_worker_connection_closed();
    }
  }

  void register_active_worker_connection(
      WorkerConnectionQueueState &queue_state,
      const std::shared_ptr<WorkerConnectionState> &connection)
  {
    if (!connection || connection->client_fd < 0)
    {
      return;
    }
    const std::lock_guard<std::mutex> lock(queue_state.mutex);
    queue_state.active_connections[connection->client_fd] = connection;
  }

  void unregister_active_worker_connection(
      WorkerConnectionQueueState &queue_state,
      int client_fd)
  {
    if (client_fd < 0)
    {
      return;
    }
    const std::lock_guard<std::mutex> lock(queue_state.mutex);
    queue_state.active_connections.erase(client_fd);
    queue_state.condition.notify_all();
  }

  bool wait_for_active_worker_connections_idle(
      WorkerConnectionQueueState &queue_state,
      std::chrono::milliseconds timeout)
  {
    std::unique_lock<std::mutex> lock(queue_state.mutex);
    return queue_state.condition.wait_for(lock, timeout, [&queue_state]()
                                          { return queue_state.active_connections.empty(); });
  }

  void interrupt_active_worker_connections(WorkerConnectionQueueState &queue_state)
  {
    std::vector<int> active_fds;
    {
      const std::lock_guard<std::mutex> lock(queue_state.mutex);
      active_fds.reserve(queue_state.active_connections.size());
      for (const auto &entry : queue_state.active_connections)
      {
        if (entry.first >= 0)
        {
          active_fds.push_back(entry.first);
        }
      }
    }
    for (int client_fd : active_fds)
    {
      shutdown(client_fd, SHUT_RDWR);
    }
  }

  struct RackExecutionIdleWaitContext
  {
    std::chrono::milliseconds timeout;
    bool drained = false;
  };

  void *wait_for_rack_execution_idle_without_gvl(void *data)
  {
    auto *context = static_cast<RackExecutionIdleWaitContext *>(data);
    context->drained = Vajra::rack::wait_for_same_process_rack_execution_idle(context->timeout);
    return nullptr;
  }

  bool wait_for_rack_execution_idle(std::chrono::milliseconds timeout)
  {
    RackExecutionIdleWaitContext context{timeout, false};
    rb_thread_call_without_gvl(
        wait_for_rack_execution_idle_without_gvl,
        &context,
        RUBY_UBF_IO,
        nullptr);
    return context.drained;
  }

  struct ActiveConnectionIdleWaitContext
  {
    WorkerConnectionQueueState *queue_state = nullptr;
    std::chrono::milliseconds timeout;
    bool drained = false;
  };

  void *wait_for_active_connections_idle_without_gvl(void *data)
  {
    auto *context = static_cast<ActiveConnectionIdleWaitContext *>(data);
    context->drained = wait_for_active_worker_connections_idle(*context->queue_state, context->timeout);
    return nullptr;
  }

  bool wait_for_active_connections_idle(
      WorkerConnectionQueueState &queue_state,
      std::chrono::milliseconds timeout)
  {
    ActiveConnectionIdleWaitContext context{&queue_state, timeout, false};
    rb_thread_call_without_gvl(
        wait_for_active_connections_idle_without_gvl,
        &context,
        RUBY_UBF_IO,
        nullptr);
    return context.drained;
  }

  void reset_worker_connection_state_for_reuse(WorkerConnectionState &connection)
  {
    connection.client_fd = -1;
    connection.buffered_bytes.clear();
    connection.buffered_bytes.shrink_to_fit();
    connection.first_request = true;
    connection.completed_requests = 0;
    connection.tls_owned_by_worker = false;
    connection.idle_started_at = std::chrono::steady_clock::now();
    connection.head_started_at = std::chrono::steady_clock::now();
    connection.head_started = false;
    connection.head_complete_buffered = false;
  }

  std::shared_ptr<WorkerConnectionState> acquire_worker_connection_state(WorkerConnectionQueueState &queue_state)
  {
    std::lock_guard<std::mutex> lock(queue_state.mutex);
    if (!queue_state.connection_pool.empty())
    {
      std::shared_ptr<WorkerConnectionState> connection = std::move(queue_state.connection_pool.back());
      queue_state.connection_pool.pop_back();
      reset_worker_connection_state_for_reuse(*connection);
      return connection;
    }
    return std::make_shared<WorkerConnectionState>();
  }

  void recycle_worker_connection_state(
      WorkerConnectionQueueState &queue_state,
      std::shared_ptr<WorkerConnectionState> connection)
  {
    if (!connection)
    {
      return;
    }
    close_worker_connection_state_and_note(*connection);
    reset_worker_connection_state_for_reuse(*connection);
    std::lock_guard<std::mutex> lock(queue_state.mutex);
    if (!queue_state.shutdown_requested && queue_state.connection_pool.size() < kMaxWarmWorkerConnectionStates)
    {
      queue_state.connection_pool.push_back(std::move(connection));
    }
  }

  bool buffered_head_complete(const std::string &buffered_bytes)
  {
    return buffered_bytes.find("\r\n\r\n") != std::string::npos;
  }

  void reset_worker_head_tracking(WorkerConnectionState &connection)
  {
    connection.head_started = false;
    connection.head_complete_buffered = false;
    connection.head_started_at = std::chrono::steady_clock::now();
  }

  void note_worker_head_started(WorkerConnectionState &connection)
  {
    if (!connection.head_started)
    {
      connection.head_started = true;
      connection.head_started_at = std::chrono::steady_clock::now();
    }
  }

  void prebuffer_ready_plain_request_head(
      const WorkerHotPathLoopContext &context,
      WorkerConnectionState &connection)
  {
    if (context.tls_context != nullptr || connection.client_fd < 0 || !connection.buffered_bytes.empty())
    {
      return;
    }

    char buffer[4096];
    for (;;)
    {
      int available_bytes = 0;
      if (ioctl(connection.client_fd, FIONREAD, &available_bytes) < 0 || available_bytes <= 0)
      {
        return;
      }

      const std::size_t bytes_to_read = std::min<std::size_t>(
          sizeof(buffer),
          static_cast<std::size_t>(available_bytes));
      const ssize_t bytes_read = recv(connection.client_fd, buffer, bytes_to_read, 0);
      if (bytes_read > 0)
      {
        note_worker_head_started(connection);
        connection.buffered_bytes.append(buffer, static_cast<std::size_t>(bytes_read));
        connection.head_complete_buffered = buffered_head_complete(connection.buffered_bytes);
        if (connection.head_complete_buffered)
        {
          return;
        }
        continue;
      }

      if (bytes_read == 0)
      {
        return;
      }

      if (errno == EINTR)
      {
        continue;
      }
      return;
    }
  }

  bool worker_partial_head_timed_out(
      const WorkerHotPathLoopContext &context,
      const WorkerConnectionState &connection)
  {
    if (!connection.head_started || connection.head_complete_buffered || connection.buffered_bytes.empty())
    {
      return false;
    }
    if (context.request_head_timeout_seconds <= 0)
    {
      return false;
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - connection.head_started_at);
    return elapsed >= std::chrono::milliseconds(context.request_head_timeout_seconds * 1000);
  }

  std::size_t worker_connection_credits(
      const Vajra::runtime::WorkerRuntimeState &worker_state,
      std::size_t max_active_connections,
      std::size_t pending_connections)
  {
    const std::size_t idle_executions = worker_state.idle_execution_count.load(std::memory_order_acquire);
    const std::size_t active_connections = worker_state.active_connections.load(std::memory_order_acquire);
    const std::size_t local_queue_depth = worker_state.local_queue_depth.load(std::memory_order_acquire);
    const std::size_t buffered_connections = active_connections + local_queue_depth + pending_connections;
    if (buffered_connections >= max_active_connections)
    {
      return 0;
    }

    const std::size_t remaining_capacity = max_active_connections - buffered_connections;
    return std::min(idle_executions + 1, remaining_capacity);
  }

  bool enqueue_worker_connection(
      WorkerConnectionQueueState &queue_state,
      std::shared_ptr<WorkerConnectionState> connection,
      std::chrono::milliseconds wait_interval)
  {
    while (!shutdown_requested_or_runtime_draining())
    {
      std::unique_lock<std::mutex> lock(queue_state.mutex);
      if (queue_state.shutdown_requested)
      {
        return false;
      }
      const std::size_t pending_connections = queue_state.pending_connections.size();
      std::size_t credits = queue_state.max_pending_connections;
      if (queue_state.capacity_gated)
      {
        if (auto *worker_state = Vajra::runtime::current_worker_runtime_state(); worker_state != nullptr)
        {
          credits = worker_connection_credits(*worker_state, queue_state.max_active_connections, pending_connections);
        }
        else
        {
          credits = 0;
        }
      }
      const bool below_pending_limit = pending_connections < queue_state.max_pending_connections;
      const bool below_active_limit = !queue_state.capacity_gated || credits > 0;
      if (below_pending_limit && below_active_limit)
      {
        queue_state.pending_connections.push_back(std::move(connection));
        Vajra::runtime::note_worker_local_queue_depth(queue_state.pending_connections.size());
        lock.unlock();
        queue_state.condition.notify_one();
        return true;
      }
      queue_state.condition.wait_for(lock, wait_interval);
    }

    return false;
  }

  bool register_idle_worker_connection(
      WorkerConnectionQueueState &queue_state,
      std::shared_ptr<WorkerConnectionState> connection)
  {
    if (!connection || connection->client_fd < 0)
    {
      return false;
    }

    std::unique_lock<std::mutex> lock(queue_state.mutex);
    if (queue_state.shutdown_requested || queue_state.reactor_fd < 0)
    {
      return false;
    }
    const int client_fd = connection->client_fd;
    connection->idle_started_at = std::chrono::steady_clock::now();
    queue_state.idle_connections[client_fd] = std::move(connection);
    const int reactor_fd = queue_state.reactor_fd;
    lock.unlock();

    if (!add_worker_reactor_fd(reactor_fd, client_fd))
    {
      const int error_number = errno;
      lock.lock();
      auto iterator = queue_state.idle_connections.find(client_fd);
      if (iterator != queue_state.idle_connections.end())
      {
        close_worker_connection_state(*iterator->second);
        queue_state.idle_connections.erase(iterator);
      }
      lock.unlock();
      Vajra::runtime::log_runtime_error(std::string("worker reactor registration failed: ") + std::strerror(error_number));
      return false;
    }
    return true;
  }

  void run_native_worker_connection_reactor(WorkerHotPathLoopContext *context)
  {
    block_ruby_reserved_signals_for_native_thread();
    if (context == nullptr || context->connection_queue_state == nullptr)
    {
      return;
    }

    Vajra::runtime::attach_current_thread_to_worker_runtime_state(static_cast<std::size_t>(context->worker_index));
    WorkerConnectionQueueState &queue_state = *context->connection_queue_state;
    std::vector<std::shared_ptr<WorkerConnectionState>> timed_out_connections;
    std::vector<int> ready_fds;
    timed_out_connections.reserve(64);
    ready_fds.reserve(128);
    for (;;)
    {
      int reactor_fd = -1;
      timed_out_connections.clear();
      {
        const std::lock_guard<std::mutex> lock(queue_state.mutex);
        if (queue_state.shutdown_requested)
        {
          break;
        }
        reactor_fd = queue_state.reactor_fd;
        const auto now = std::chrono::steady_clock::now();
        for (auto iterator = queue_state.idle_connections.begin(); iterator != queue_state.idle_connections.end();)
        {
          const int timeout_seconds = iterator->second->first_request
                                          ? context->first_data_timeout_seconds
                                          : context->persistent_timeout_seconds;
          const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
              now - iterator->second->idle_started_at);
          const auto timeout = std::chrono::milliseconds(timeout_seconds * 1000);
          if (timeout_seconds > 0 && elapsed >= timeout)
          {
            timed_out_connections.push_back(std::move(iterator->second));
            iterator = queue_state.idle_connections.erase(iterator);
          }
          else
          {
            ++iterator;
          }
        }
      }
      for (const auto &connection : timed_out_connections)
      {
        if (connection)
        {
          remove_worker_reactor_fd(reactor_fd, connection->client_fd);
          recycle_worker_connection_state(queue_state, connection);
        }
      }
      if (reactor_fd < 0)
      {
        break;
      }

      try
      {
        wait_worker_reactor_events(reactor_fd, 100, ready_fds);
      }
      catch (const std::exception &error)
      {
        if (shutdown_requested_or_runtime_draining())
        {
          break;
        }
        Vajra::runtime::log_runtime_error(error.what());
        continue;
      }

      for (int client_fd : ready_fds)
      {
        std::shared_ptr<WorkerConnectionState> connection;
        {
          std::lock_guard<std::mutex> lock(queue_state.mutex);
          if (queue_state.shutdown_requested)
          {
            break;
          }
          auto iterator = queue_state.idle_connections.find(client_fd);
          if (iterator == queue_state.idle_connections.end())
          {
            continue;
          }
          connection = std::move(iterator->second);
          queue_state.idle_connections.erase(iterator);
        }
        remove_worker_reactor_fd(reactor_fd, client_fd);
        prebuffer_ready_plain_request_head(*context, *connection);
        if (!enqueue_worker_connection(queue_state, connection, std::chrono::milliseconds(1)))
        {
          recycle_worker_connection_state(queue_state, connection);
        }
      }
    }
  }

  void *run_worker_connection_receiver_without_gvl(void *data)
  {
    auto *context = static_cast<WorkerHotPathLoopContext *>(data);
    if (context == nullptr || context->connection_queue_state == nullptr)
    {
      return nullptr;
    }

    Vajra::runtime::attach_current_thread_to_worker_runtime_state(static_cast<std::size_t>(context->worker_index));

    auto &queue_state = *context->connection_queue_state;
    for (;;)
    {
      {
        const std::lock_guard<std::mutex> lock(queue_state.mutex);
        if (queue_state.shutdown_requested)
        {
          break;
        }
      }
      pollfd control_descriptor{context->control_fd, POLLIN | POLLHUP | POLLERR, 0};
      const int poll_result = poll(&control_descriptor, 1, 100);
      if (poll_result == 0)
      {
        continue;
      }
      if (poll_result < 0)
      {
        if (errno == EINTR)
        {
          continue;
        }
        if (shutdown_requested_or_runtime_draining())
        {
          break;
        }
        Vajra::runtime::log_runtime_error(std::string("worker connection receiver poll failed: ") + std::strerror(errno));
        break;
      }
      if ((control_descriptor.revents & (POLLIN | POLLHUP | POLLERR)) == 0)
      {
        continue;
      }

      std::optional<int> received_client_fd;
      try
      {
        received_client_fd = receive_single_file_descriptor(context->control_fd);
      }
      catch (const std::exception &error)
      {
        if (shutdown_requested_or_runtime_draining())
        {
          {
            const std::lock_guard<std::mutex> lock(queue_state.mutex);
            queue_state.shutdown_requested = true;
          }
          queue_state.condition.notify_all();
          break;
        }
        Vajra::runtime::log_runtime_error(std::string("worker connection receiver failed: ") + error.what());
        continue;
      }

      if (!received_client_fd.has_value())
      {
        {
          const std::lock_guard<std::mutex> lock(queue_state.mutex);
          queue_state.shutdown_requested = true;
        }
        queue_state.condition.notify_all();
        break;
      }

      auto connection = acquire_worker_connection_state(queue_state);
      connection->client_fd = *received_client_fd;
      Vajra::runtime::note_worker_connection_opened();
      if (!register_idle_worker_connection(queue_state, connection))
      {
        recycle_worker_connection_state(queue_state, connection);
        continue;
      }
    }

    return nullptr;
  }

  void run_native_worker_connection_receiver(WorkerHotPathLoopContext *context)
  {
    block_ruby_reserved_signals_for_native_thread();
    run_worker_connection_receiver_without_gvl(context);
  }

  std::shared_ptr<WorkerConnectionState> dequeue_worker_connection(WorkerConnectionQueueState &queue_state)
  {
    std::unique_lock<std::mutex> lock(queue_state.mutex);
    queue_state.condition.wait(lock, [&queue_state]
                               { return queue_state.shutdown_requested || !queue_state.pending_connections.empty(); });
    if (queue_state.pending_connections.empty())
    {
      return nullptr;
    }

    std::shared_ptr<WorkerConnectionState> connection = std::move(queue_state.pending_connections.front());
    queue_state.pending_connections.pop_front();
    Vajra::runtime::note_worker_local_queue_depth(queue_state.pending_connections.size());
    lock.unlock();
    queue_state.condition.notify_one();
    return connection;
  }

  void handle_worker_connection(
      WorkerHotPathLoopContext &context,
      const std::shared_ptr<WorkerConnectionState> &connection_state)
  {
    if (!connection_state || connection_state->client_fd < 0)
    {
      return;
    }

    const int client_fd = connection_state->client_fd;
    Vajra::response::ResponseWriter::prepare_client_socket(client_fd);
    bool close_connection = true;
    try
    {
      Vajra::request::SocketContext socket_context =
          socket_context_for_client_fd(client_fd, context.fallback_port);
      if (context.tls_context != nullptr)
      {
        Vajra::transport::TlsConnection connection(client_fd, *context.tls_context);
        connection.handshake();
        socket_context.scheme = "https";
        if (connection.protocol() == "h2")
        {
          if (!context.http2_enabled)
          {
            throw std::runtime_error("ALPN selected h2 while HTTP/2 is disabled");
          }
          Vajra::request::Http2Session session(
              connection,
              socket_context,
              context.http2_config,
              context.request_executor,
              context.request_processor->http2_execution_pool());
          session.run();
        }
        else
        {
          const Vajra::request::RequestProcessingOutcome outcome =
              context.request_processor->handle(connection, socket_context);
          if (outcome == Vajra::request::RequestProcessingOutcome::hijacked)
          {
            connection_state->client_fd = -1;
            close_connection = false;
            Vajra::runtime::note_worker_connection_closed();
          }
        }
      }
      else
      {
        Vajra::transport::PlainConnection connection(client_fd);
        const H2cPrefaceProbeOutcome h2c_probe = probe_h2c_prior_knowledge(
            connection,
            *connection_state,
            context.first_data_timeout_seconds);
        if (h2c_probe == H2cPrefaceProbeOutcome::prior_knowledge)
        {
          if (context.http2_enabled)
          {
            socket_context.scheme = "http";
            Vajra::request::Http2Session session(
                connection,
                socket_context,
                context.http2_config,
                context.request_executor,
                context.request_processor->http2_execution_pool(),
                std::move(connection_state->buffered_bytes));
            connection_state->buffered_bytes.clear();
            session.run();
          }
          else
          {
            write_h2c_bad_request(connection);
          }
          close_connection = true;
        }
        else if (h2c_probe == H2cPrefaceProbeOutcome::await_more)
        {
          note_worker_head_started(*connection_state);
          connection_state->head_complete_buffered = false;
          close_connection = !register_idle_worker_connection(*context.connection_queue_state, connection_state);
        }
        else if (h2c_probe == H2cPrefaceProbeOutcome::invalid_preface)
        {
          if (context.http2_enabled)
          {
            write_h2c_protocol_goaway(connection);
          }
          else
          {
            write_h2c_bad_request(connection);
          }
          close_connection = true;
        }
        else if (h2c_probe == H2cPrefaceProbeOutcome::closed)
        {
          close_connection = true;
        }
        else if (worker_partial_head_timed_out(context, *connection_state))
        {
          close_connection = true;
        }
        else
        {
          const bool force_close_after_response =
              context.max_keepalive_requests > 0 &&
              connection_state->completed_requests + 1 >= context.max_keepalive_requests;
          Vajra::request::RequestProcessingResult result = context.request_processor->handle_one(
              connection,
              socket_context,
              std::move(connection_state->buffered_bytes),
              connection_state->first_request,
              force_close_after_response);
          if (result.outcome == Vajra::request::RequestProcessingOutcome::keep_alive)
          {
            ++connection_state->completed_requests;
            connection_state->buffered_bytes = std::move(result.buffered_bytes);
            connection_state->first_request = result.first_request;
            if (!connection_state->buffered_bytes.empty())
            {
              note_worker_head_started(*connection_state);
              connection_state->head_complete_buffered = buffered_head_complete(connection_state->buffered_bytes);
            }
            else
            {
              reset_worker_head_tracking(*connection_state);
            }
            if (!connection_state->buffered_bytes.empty())
            {
              close_connection = !enqueue_worker_connection(
                  *context.connection_queue_state,
                  connection_state,
                  std::chrono::milliseconds(1));
            }
            else
            {
              close_connection = !register_idle_worker_connection(*context.connection_queue_state, connection_state);
            }
          }
          else if (result.outcome == Vajra::request::RequestProcessingOutcome::await_read)
          {
            connection_state->buffered_bytes = std::move(result.buffered_bytes);
            connection_state->first_request = result.first_request;
            if (!connection_state->buffered_bytes.empty())
            {
              note_worker_head_started(*connection_state);
              connection_state->head_complete_buffered = buffered_head_complete(connection_state->buffered_bytes);
            }
            else
            {
              reset_worker_head_tracking(*connection_state);
            }
            close_connection = !register_idle_worker_connection(*context.connection_queue_state, connection_state);
          }
          else if (result.outcome == Vajra::request::RequestProcessingOutcome::hijacked)
          {
            connection_state->client_fd = -1;
            close_connection = false;
            Vajra::runtime::note_worker_connection_closed();
          }
        }
      }
    }
    catch (const std::exception &error)
    {
      Vajra::runtime::log_runtime_error(std::string("worker connection failed: ") + error.what());
    }
    catch (...)
    {
      Vajra::runtime::log_runtime_error("worker connection failed with an unknown native error");
    }

    if (close_connection)
    {
      if (context.connection_queue_state != nullptr)
      {
        recycle_worker_connection_state(*context.connection_queue_state, connection_state);
      }
      else
      {
        close_worker_connection_state_and_note(*connection_state);
      }
    }
    maybe_trim_linux_malloc();
  }

  void *run_worker_connection_loop_without_gvl(void *data)
  {
    auto *context = static_cast<WorkerHotPathLoopContext *>(data);
    if (context == nullptr || context->request_processor == nullptr)
    {
      return nullptr;
    }

    Vajra::runtime::attach_current_thread_to_worker_runtime_state(static_cast<std::size_t>(context->worker_index));

    while (!shutdown_requested_or_runtime_draining())
    {
      std::shared_ptr<WorkerConnectionState> connection;
      try
      {
        if (context->connection_queue_state == nullptr)
        {
          break;
        }

        connection = dequeue_worker_connection(*context->connection_queue_state);
        if (!connection)
        {
          break;
        }
        Vajra::runtime::note_worker_dispatch_received();
      }
      catch (const std::exception &error)
      {
        if (connection)
        {
          recycle_worker_connection_state(*context->connection_queue_state, connection);
        }
        Vajra::runtime::log_runtime_error(std::string("worker connection receive failed: ") + error.what());
        continue;
      }

      const int active_client_fd = connection->client_fd;
      register_active_worker_connection(*context->connection_queue_state, connection);
      handle_worker_connection(*context, connection);
      unregister_active_worker_connection(*context->connection_queue_state, active_client_fd);
    }

    return nullptr;
  }

  void run_native_worker_connection_loop(WorkerHotPathLoopContext *context)
  {
    block_ruby_reserved_signals_for_native_thread();
    run_worker_connection_loop_without_gvl(context);
  }

  [[noreturn]] void exit_worker_bootstrap_failure(
      int write_fd,
      const Vajra::runtime::BootDiagnostic &diagnostic,
      int exit_code)
  {
    try
    {
      report_worker_boot_failed(write_fd, diagnostic);
    }
    catch (...)
    {
    }

    close(write_fd);
    Vajra::runtime::stop_runtime_tracing_worker();
    Vajra::runtime::stop_runtime_logging_worker();
    _exit(exit_code);
  }

  bool worker_has_exited(const std::shared_ptr<Vajra::runtime::SharedWorkerState> &worker_state)
  {
    return worker_state->lifecycle_state.load(std::memory_order_acquire) == Vajra::runtime::WorkerLifecycleState::exited;
  }

  bool process_is_alive(pid_t pid)
  {
    if (pid <= 0)
    {
      return false;
    }

    if (kill(pid, 0) == 0)
    {
      return true;
    }

    return errno != ESRCH;
  }

  std::int64_t steady_clock_nanoseconds()
  {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }

  std::int64_t steady_clock_nanoseconds_after(std::chrono::steady_clock::duration offset)
  {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               (std::chrono::steady_clock::now() + offset).time_since_epoch())
        .count();
  }

  void mark_lifecycle_transition(
      const std::shared_ptr<Vajra::runtime::SharedWorkerState> &worker_state,
      Vajra::runtime::WorkerLifecycleState lifecycle_state)
  {
    worker_state->lifecycle_state.store(lifecycle_state, std::memory_order_release);
    worker_state->last_lifecycle_transition_nanoseconds.store(steady_clock_nanoseconds(), std::memory_order_release);
  }

  void mark_recovery_transition(
      const std::shared_ptr<Vajra::runtime::SharedWorkerState> &worker_state,
      Vajra::runtime::WorkerRecoveryState recovery_state)
  {
    worker_state->recovery_state.store(recovery_state, std::memory_order_release);
  }

  bool health_requires_quarantine(Vajra::runtime::WorkerHealthState state)
  {
    return state == Vajra::runtime::WorkerHealthState::overloaded ||
           state == Vajra::runtime::WorkerHealthState::degraded ||
           state == Vajra::runtime::WorkerHealthState::suspect ||
           state == Vajra::runtime::WorkerHealthState::wedged;
  }

  Vajra::runtime::HealthPolicy health_policy_for(const Vajra::runtime::RuntimeConfig &config)
  {
    return Vajra::runtime::HealthPolicy{
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::milliseconds(250)).count(),
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::seconds(std::max(2, config.worker_timeout_seconds / 2)))
            .count(),
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::seconds(std::max(5, config.worker_timeout_seconds / 2)))
            .count(),
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::seconds(std::max(10, config.worker_timeout_seconds)))
            .count(),
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::seconds(std::max(30, config.worker_timeout_seconds * 2)))
            .count(),
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::seconds(std::max(1, config.worker_timeout_seconds)))
            .count()};
  }

  std::chrono::steady_clock::duration watcher_sleep_interval(
      const std::vector<std::shared_ptr<Vajra::runtime::SharedWorkerState>> &worker_states)
  {
    const std::int64_t now_nanoseconds = steady_clock_nanoseconds();
    std::optional<std::int64_t> earliest_deadline;
    for (const auto &worker_state : worker_states)
    {
      if (!worker_state->timeout_escalation_pending.load(std::memory_order_acquire))
      {
        continue;
      }

      const std::int64_t deadline = worker_state->timeout_kill_deadline_nanoseconds.load(std::memory_order_acquire);
      if (deadline == 0)
      {
        continue;
      }
      if (!earliest_deadline.has_value() || deadline < *earliest_deadline)
      {
        earliest_deadline = deadline;
      }
    }

    if (!earliest_deadline.has_value())
    {
      return kWorkerExitWatcherIdlePollInterval;
    }
    if (*earliest_deadline <= now_nanoseconds)
    {
      return std::chrono::steady_clock::duration::zero();
    }

    return std::chrono::nanoseconds(*earliest_deadline - now_nanoseconds);
  }

  bool signal_process_with_retry(pid_t pid, int signal_number, const char *error_message)
  {
    int interrupted_attempts = 0;
    for (;;)
    {
      if (kill(pid, signal_number) == 0)
      {
        return true;
      }
      if (errno == ESRCH)
      {
        return false;
      }
      if (errno == EINTR && interrupted_attempts < kSignalRetryLimit)
      {
        ++interrupted_attempts;
        continue;
      }
      if (errno == EINTR)
      {
        throw std::runtime_error(std::string(error_message) + ": interrupted too many times");
      }
      throw std::runtime_error(error_message);
    }
  }
}

Vajra::runtime::NativeRuntime &Vajra::runtime::NativeRuntime::instance()
{
  static NativeRuntime runtime;
  return runtime;
}

bool Vajra::runtime::NativeRuntime::shutdown_requested()
{
  return shutting_down != 0;
}

std::vector<std::shared_ptr<Vajra::runtime::SharedWorkerState>> Vajra::runtime::NativeRuntime::worker_states() const
{
  const std::lock_guard<std::mutex> lock(server_mutex_);
  return worker_states_;
}

void Vajra::runtime::NativeRuntime::begin_runtime_shutdown()
{
  std::lock_guard<std::mutex> lock(server_mutex_);
  if (runtime_shutdown_started_)
  {
    return;
  }

  runtime_shutdown_started_ = true;
  log_runtime_shutdown_begin();
}

void Vajra::runtime::NativeRuntime::forward_shutdown_to_workers()
{
  (void)stop_worker_processes();
}

bool Vajra::runtime::NativeRuntime::runtime_running() const
{
  const std::lock_guard<std::mutex> lock(server_mutex_);
  return !worker_states_.empty() || server_instance_ || worker_startup_in_progress_;
}

bool Vajra::runtime::NativeRuntime::try_begin_startup()
{
  const std::lock_guard<std::mutex> lock(server_mutex_);
  if (!worker_states_.empty() || server_instance_ || worker_startup_in_progress_)
  {
    return false;
  }

  worker_startup_in_progress_ = true;
  return true;
}

std::shared_ptr<Vajra::runtime::SharedWorkerState> Vajra::runtime::NativeRuntime::register_worker_runtime(
    std::size_t worker_index,
    pid_t pid,
    std::vector<int> control_channel_fds)
{
  const auto worker_state =
      std::make_shared<SharedWorkerState>(worker_index, pid, std::move(control_channel_fds));
  {
    const std::lock_guard<std::mutex> lock(server_mutex_);
    worker_states_.push_back(worker_state);
    worker_startup_in_progress_ = false;
  }
  worker_state->last_progress_nanoseconds.store(steady_clock_nanoseconds(), std::memory_order_release);
  worker_state->last_lifecycle_transition_nanoseconds.store(steady_clock_nanoseconds(), std::memory_order_release);
  worker_state->last_health_transition_nanoseconds.store(steady_clock_nanoseconds(), std::memory_order_release);
  worker_state->control_channel_count.store(worker_state->control_channel_fds.size(), std::memory_order_release);
  worker_state->request_channel_count.store(worker_state->request_channel_fds.size(), std::memory_order_release);
  worker_state->idle_execution_count.store(worker_spawn_config_.max_threads, std::memory_order_release);
  Vajra::runtime::mark_worker_lifecycle(worker_index, WorkerLifecycleState::booting);
  Vajra::runtime::mark_worker_available(worker_index, false);
  if (debug_logging_.load(std::memory_order_acquire))
  {
    log_worker_lifecycle_event(
        "worker_registered",
        worker_state->worker_index,
        pid,
        WorkerLifecycleState::booting,
        worker_state->health_state.load(std::memory_order_acquire),
        worker_state->recovery_state.load(std::memory_order_acquire),
        false,
        WorkerExitClassification::none,
        worker_state->terminal_replacement_failure.load(std::memory_order_acquire),
        false,
        0);
  }
  worker_state_changed_.notify_all();
  return worker_state;
}

void Vajra::runtime::NativeRuntime::mark_worker_ready(const std::shared_ptr<SharedWorkerState> &worker_state)
{
  worker_state->replacement_needed.store(false, std::memory_order_release);
  worker_state->recovery_requested.store(false, std::memory_order_release);
  worker_state->expected_shutdown.store(false, std::memory_order_release);
  worker_state->timeout_escalation_pending.store(false, std::memory_order_release);
  worker_state->timeout_kill_deadline_nanoseconds.store(0, std::memory_order_release);
  worker_state->timeout_handling_started.store(false, std::memory_order_release);
  worker_state->available.store(true, std::memory_order_release);
  worker_state->terminal_replacement_failure.store(false, std::memory_order_release);
  worker_state->overload_started_nanoseconds.store(0, std::memory_order_release);
  worker_state->recovery_deadline_nanoseconds.store(0, std::memory_order_release);
  mark_recovery_transition(worker_state, WorkerRecoveryState::none);
  mark_lifecycle_transition(worker_state, WorkerLifecycleState::ready);
  worker_state->health_state.store(WorkerHealthState::healthy, std::memory_order_release);
  worker_state->last_health_transition_nanoseconds.store(steady_clock_nanoseconds(), std::memory_order_release);
  Vajra::runtime::mark_worker_lifecycle(worker_state->worker_index, WorkerLifecycleState::ready);
  Vajra::runtime::mark_worker_recovery(worker_state->worker_index, WorkerRecoveryState::none);
  Vajra::runtime::mark_worker_health(worker_state->worker_index, WorkerHealthState::healthy);
  Vajra::runtime::mark_worker_available(worker_state->worker_index, true);
  if (debug_logging_.load(std::memory_order_acquire))
  {
    log_worker_lifecycle_event(
        "worker_ready",
        worker_state->worker_index,
        worker_state->pid.load(std::memory_order_acquire),
        WorkerLifecycleState::ready,
        worker_state->health_state.load(std::memory_order_acquire),
        worker_state->recovery_state.load(std::memory_order_acquire),
        true,
        WorkerExitClassification::none,
        worker_state->terminal_replacement_failure.load(std::memory_order_acquire),
        false,
        0);
  }
  worker_state_changed_.notify_all();
}

void Vajra::runtime::NativeRuntime::mark_worker_stopping(const std::shared_ptr<SharedWorkerState> &worker_state)
{
  WorkerLifecycleState previous_state = worker_state->lifecycle_state.load(std::memory_order_acquire);
  while (previous_state != WorkerLifecycleState::stopping &&
         previous_state != WorkerLifecycleState::exited)
  {
    if (worker_state->lifecycle_state.compare_exchange_weak(
            previous_state,
            WorkerLifecycleState::stopping,
            std::memory_order_acq_rel,
            std::memory_order_acquire))
    {
      break;
    }
  }

  if (previous_state == WorkerLifecycleState::stopping || previous_state == WorkerLifecycleState::exited)
  {
    return;
  }

  worker_state->last_lifecycle_transition_nanoseconds.store(steady_clock_nanoseconds(), std::memory_order_release);
  worker_state->available.store(false, std::memory_order_release);
  worker_state->health_state.store(WorkerHealthState::degraded, std::memory_order_release);
  mark_recovery_transition(worker_state, WorkerRecoveryState::draining);
  worker_state->last_health_transition_nanoseconds.store(steady_clock_nanoseconds(), std::memory_order_release);
  Vajra::runtime::mark_worker_lifecycle(worker_state->worker_index, WorkerLifecycleState::stopping);
  Vajra::runtime::mark_worker_recovery(worker_state->worker_index, WorkerRecoveryState::draining);
  Vajra::runtime::mark_worker_health(worker_state->worker_index, WorkerHealthState::degraded);
  Vajra::runtime::mark_worker_available(worker_state->worker_index, false);
  if (debug_logging_.load(std::memory_order_acquire))
  {
    log_worker_lifecycle_event(
        "worker_stopping",
        worker_state->worker_index,
        worker_state->pid.load(std::memory_order_acquire),
        WorkerLifecycleState::stopping,
        worker_state->health_state.load(std::memory_order_acquire),
        worker_state->recovery_state.load(std::memory_order_acquire),
        false,
        worker_state->last_exit_classification.load(std::memory_order_acquire),
        worker_state->terminal_replacement_failure.load(std::memory_order_acquire),
        worker_state->replacement_needed.load(std::memory_order_acquire),
        worker_state->last_exit_detail.load(std::memory_order_acquire));
  }
  worker_state_changed_.notify_all();
}

void Vajra::runtime::NativeRuntime::mark_worker_exit(
    const std::shared_ptr<SharedWorkerState> &worker_state,
    WorkerExitClassification exit_classification,
    int exit_detail)
{
  const WorkerLifecycleState previous_state =
      worker_state->lifecycle_state.exchange(WorkerLifecycleState::exited, std::memory_order_acq_rel);
  if (previous_state == WorkerLifecycleState::exited)
  {
    return;
  }

  worker_state->last_lifecycle_transition_nanoseconds.store(steady_clock_nanoseconds(), std::memory_order_release);
  worker_state->available.store(false, std::memory_order_release);
  worker_state->last_exit_classification.store(exit_classification, std::memory_order_release);
  worker_state->last_exit_detail.store(exit_detail, std::memory_order_release);
  worker_state->timeout_escalation_pending.store(false, std::memory_order_release);
  worker_state->timeout_kill_deadline_nanoseconds.store(0, std::memory_order_release);
  worker_state->timeout_handling_started.store(false, std::memory_order_release);
  worker_state->active_execution_count.store(0, std::memory_order_release);
  worker_state->idle_execution_count.store(0, std::memory_order_release);
  worker_state->local_queue_depth.store(0, std::memory_order_release);
  worker_state->oldest_local_queue_age_nanoseconds.store(0, std::memory_order_release);
  worker_state->health_state.store(
      exit_classification == WorkerExitClassification::expected_shutdown
          ? WorkerHealthState::degraded
          : WorkerHealthState::wedged,
      std::memory_order_release);
  worker_state->last_health_transition_nanoseconds.store(steady_clock_nanoseconds(), std::memory_order_release);
  if (exit_classification == WorkerExitClassification::unexpected_exit ||
      exit_classification == WorkerExitClassification::unexpected_signal ||
      exit_classification == WorkerExitClassification::unexpected_status)
  {
    worker_state->unexpected_exit_count.fetch_add(1, std::memory_order_acq_rel);
    worker_state->last_unexpected_exit_nanoseconds.store(steady_clock_nanoseconds(), std::memory_order_release);
  }
  const bool replacement_needed =
      exit_classification != WorkerExitClassification::none &&
      exit_classification != WorkerExitClassification::expected_shutdown;
  worker_state->replacement_needed.store(replacement_needed, std::memory_order_release);
  worker_state->recovery_requested.store(false, std::memory_order_release);
  worker_state->spawned_by_worker_spawner.store(false, std::memory_order_release);
  mark_recovery_transition(
      worker_state,
      replacement_needed ? WorkerRecoveryState::rejoin_pending : WorkerRecoveryState::none);
  close_worker_control_channels(worker_state);
  Vajra::runtime::mark_worker_lifecycle(worker_state->worker_index, WorkerLifecycleState::exited);
  Vajra::runtime::mark_worker_recovery(
      worker_state->worker_index,
      replacement_needed ? WorkerRecoveryState::rejoin_pending : WorkerRecoveryState::none);
  Vajra::runtime::mark_worker_health(
      worker_state->worker_index,
      exit_classification == WorkerExitClassification::expected_shutdown
          ? WorkerHealthState::degraded
          : WorkerHealthState::wedged);
  Vajra::runtime::mark_worker_available(worker_state->worker_index, false);
  Vajra::runtime::mark_worker_unexpected_exit(
      worker_state->worker_index,
      worker_state->unexpected_exit_count.load(std::memory_order_acquire),
      worker_state->last_unexpected_exit_nanoseconds.load(std::memory_order_acquire));
  if (debug_logging_.load(std::memory_order_acquire))
  {
    log_worker_lifecycle_event(
        "worker_exited",
        worker_state->worker_index,
        worker_state->pid.load(std::memory_order_acquire),
        WorkerLifecycleState::exited,
        worker_state->health_state.load(std::memory_order_acquire),
        worker_state->recovery_state.load(std::memory_order_acquire),
        false,
        exit_classification,
        worker_state->terminal_replacement_failure.load(std::memory_order_acquire),
        replacement_needed,
        exit_detail);
    if (replacement_needed)
    {
      log_worker_lifecycle_event(
          "worker_replacement_pending",
          worker_state->worker_index,
          worker_state->pid.load(std::memory_order_acquire),
          WorkerLifecycleState::exited,
          worker_state->health_state.load(std::memory_order_acquire),
          worker_state->recovery_state.load(std::memory_order_acquire),
          false,
          exit_classification,
          worker_state->terminal_replacement_failure.load(std::memory_order_acquire),
          true,
          exit_detail);
    }
  }
  worker_state_changed_.notify_all();
}

void Vajra::runtime::NativeRuntime::prepare_worker_replacement(const std::shared_ptr<SharedWorkerState> &worker_state)
{
  worker_state->available.store(false, std::memory_order_release);
  worker_state->replacement_needed.store(false, std::memory_order_release);
  worker_state->expected_shutdown.store(false, std::memory_order_release);
  worker_state->timeout_escalation_pending.store(false, std::memory_order_release);
  worker_state->timeout_kill_deadline_nanoseconds.store(0, std::memory_order_release);
  worker_state->timeout_handling_started.store(false, std::memory_order_release);
  worker_state->control_channels_closed.store(false, std::memory_order_release);
  worker_state->request_channels_closed.store(false, std::memory_order_release);
  worker_state->active_execution_count.store(0, std::memory_order_release);
  worker_state->idle_execution_count.store(worker_spawn_config_.max_threads, std::memory_order_release);
  worker_state->local_queue_depth.store(0, std::memory_order_release);
  worker_state->oldest_local_queue_age_nanoseconds.store(0, std::memory_order_release);
  worker_state->unexpected_exit_count.store(0, std::memory_order_release);
  worker_state->last_unexpected_exit_nanoseconds.store(0, std::memory_order_release);
  worker_state->last_exit_classification.store(WorkerExitClassification::none, std::memory_order_release);
  worker_state->last_exit_detail.store(0, std::memory_order_release);
  worker_state->terminal_replacement_failure.store(false, std::memory_order_release);
  worker_state->spawned_by_worker_spawner.store(false, std::memory_order_release);
  worker_state->overload_started_nanoseconds.store(0, std::memory_order_release);
  worker_state->recovery_deadline_nanoseconds.store(0, std::memory_order_release);
  worker_state->recovery_requested.store(false, std::memory_order_release);
  worker_state->health_state.store(WorkerHealthState::healthy, std::memory_order_release);
  worker_state->last_health_transition_nanoseconds.store(steady_clock_nanoseconds(), std::memory_order_release);
  mark_recovery_transition(worker_state, WorkerRecoveryState::replacing);
  mark_lifecycle_transition(worker_state, WorkerLifecycleState::booting);
  Vajra::runtime::mark_worker_lifecycle(worker_state->worker_index, WorkerLifecycleState::booting);
  Vajra::runtime::mark_worker_recovery(worker_state->worker_index, WorkerRecoveryState::replacing);
  Vajra::runtime::mark_worker_health(worker_state->worker_index, WorkerHealthState::healthy);
  Vajra::runtime::mark_worker_available(worker_state->worker_index, false);
}

void Vajra::runtime::NativeRuntime::close_worker_request_channels(const std::shared_ptr<SharedWorkerState> &worker_state)
{
  close_worker_control_channels(worker_state);
}

void Vajra::runtime::NativeRuntime::close_worker_control_channels(const std::shared_ptr<SharedWorkerState> &worker_state)
{
  bool expected = false;
  if (!worker_state->control_channels_closed.compare_exchange_strong(expected, true))
  {
    return;
  }

  std::vector<int> control_channel_fds;
  {
    const std::lock_guard<std::mutex> lock(worker_state->control_channel_mutex);
    control_channel_fds = worker_state->control_channel_fds;
  }

  for (int control_channel_fd : control_channel_fds)
  {
    shutdown(control_channel_fd, SHUT_RDWR);
    close_fd_if_open(control_channel_fd);
  }
}

bool Vajra::runtime::NativeRuntime::start_worker_spawner(const WorkerSpawnConfig &spawn_config)
{
  int control_channels[2] = {-1, -1};
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, control_channels) != 0)
  {
    throw std::runtime_error(
        std::string("worker spawner control channel creation failed: ") + std::strerror(errno));
  }

#if defined(__APPLE__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
  const pid_t pid = fork();
#if defined(__APPLE__)
#pragma clang diagnostic pop
#endif
  if (pid < 0)
  {
    const int error_number = errno;
    close_fd_if_open(control_channels[0]);
    close_fd_if_open(control_channels[1]);
    throw std::runtime_error(
        std::string("worker spawner fork failed: ") + std::strerror(error_number));
  }

  if (pid == 0)
  {
    close_fd_if_open(control_channels[0]);
    close_fd_if_open(Vajra::runtime::runtime_listener_fd());
    Vajra::runtime::set_runtime_listener_fd(-1);
    run_worker_spawner(control_channels[1], spawn_config);
  }

  close_fd_if_open(control_channels[1]);
  {
    const std::lock_guard<std::mutex> lock(server_mutex_);
    worker_spawner_pid_ = pid;
    worker_spawner_fd_ = control_channels[0];
  }
  return true;
}

void Vajra::runtime::NativeRuntime::stop_worker_spawner()
{
  int control_fd = -1;
  pid_t pid = -1;
  {
    const std::lock_guard<std::mutex> lock(server_mutex_);
    control_fd = worker_spawner_fd_;
    pid = worker_spawner_pid_;
    worker_spawner_fd_ = -1;
    worker_spawner_pid_ = -1;
  }

  if (control_fd >= 0)
  {
    const auto command = static_cast<std::uint8_t>(WorkerSpawnerCommand::shutdown);
    try
    {
      write_all_or_throw(control_fd, &command, sizeof(command));
    }
    catch (...)
    {
    }
    close_fd_if_open(control_fd);
  }

  if (pid > 0)
  {
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
    {
    }
  }
}

[[noreturn]] void Vajra::runtime::NativeRuntime::run_worker_spawner(
    int control_fd,
    const WorkerSpawnConfig &spawn_config)
{
  for (;;)
  {
    for (;;)
    {
      int status = 0;
      const pid_t reaped_pid = waitpid(-1, &status, WNOHANG);
      if (reaped_pid > 0)
      {
        continue;
      }
      if (reaped_pid < 0 && errno == EINTR)
      {
        continue;
      }
      break;
    }

    struct pollfd control_poll;
    std::memset(&control_poll, 0, sizeof(control_poll));
    control_poll.fd = control_fd;
    control_poll.events = POLLIN;
    int poll_result = -1;
    while ((poll_result = poll(&control_poll, 1, 50)) < 0)
    {
      if (errno == EINTR)
      {
        continue;
      }
      close_fd_if_open(control_fd);
      _exit(1);
    }
    if (poll_result == 0)
    {
      continue;
    }

    std::uint8_t command = 0;
    if (!read_exact_or_eof(control_fd, &command, sizeof(command)))
    {
      close_fd_if_open(control_fd);
      _exit(0);
    }

    if (command == static_cast<std::uint8_t>(WorkerSpawnerCommand::shutdown))
    {
      close_fd_if_open(control_fd);
      _exit(0);
    }

    if (command != static_cast<std::uint8_t>(WorkerSpawnerCommand::spawn))
    {
      close_fd_if_open(control_fd);
      _exit(1);
    }

    std::uint32_t worker_index = 0;
    if (!read_exact_or_eof(control_fd, &worker_index, sizeof(worker_index)))
    {
      close_fd_if_open(control_fd);
      _exit(1);
    }

    pid_t pid = -1;
    std::vector<int> parent_control_channels;
    BootDiagnostic diagnostic{
        "worker_replacement_failed",
        "boot",
        "worker spawner replacement bootstrap failed"};
    WorkerBootstrapStatus status = WorkerBootstrapStatus::ready;

    try
    {
      if (!spawn_worker_from_single_thread(worker_index, spawn_config, pid, parent_control_channels, diagnostic, control_fd))
      {
        status = WorkerBootstrapStatus::failed;
      }
    }
    catch (const std::exception &error)
    {
      status = WorkerBootstrapStatus::failed;
      diagnostic = BootDiagnostic{
          "worker_replacement_failed",
          "spawn",
          error.what()};
    }

    const WorkerSpawnerResponseHeader header{
        status,
        static_cast<std::int32_t>(pid),
        static_cast<std::uint32_t>(parent_control_channels.size())};
    write_all_or_throw(control_fd, &header, sizeof(header));

    if (status == WorkerBootstrapStatus::ready)
    {
      send_file_descriptors_or_throw(control_fd, parent_control_channels);
      for (int fd : parent_control_channels)
      {
        close_fd_if_open(fd);
      }
      continue;
    }

    write_string_payload(control_fd, diagnostic.code);
    write_string_payload(control_fd, diagnostic.category);
    write_string_payload(control_fd, diagnostic.message);
    for (int fd : parent_control_channels)
    {
      close_fd_if_open(fd);
    }
  }
}

bool Vajra::runtime::NativeRuntime::spawn_worker_from_single_thread(
    std::size_t worker_index,
    const WorkerSpawnConfig &spawn_config,
    pid_t &pid,
    std::vector<int> &parent_control_channels,
    BootDiagnostic &failure_diagnostic,
    int inherited_control_fd)
{
  int readiness_pipe[2] = {-1, -1};
  if (pipe(readiness_pipe) != 0)
  {
    throw std::runtime_error(
        std::string("worker bootstrap pipe creation failed: ") + std::strerror(errno));
  }

  std::vector<std::array<int, 2>> control_channels;
  control_channels.reserve(1);
  try
  {
    std::array<int, 2> control_channel = {-1, -1};
    if (socketpair(AF_UNIX, worker_control_socket_type(), 0, control_channel.data()) != 0)
    {
      throw std::runtime_error(
          std::string("worker control channel creation failed: ") + std::strerror(errno));
    }
    control_channels.push_back(control_channel);
  }
  catch (...)
  {
    close_fd_if_open(readiness_pipe[0]);
    close_fd_if_open(readiness_pipe[1]);
    for (const auto &pair : control_channels)
    {
      close_fd_if_open(pair[0]);
      close_fd_if_open(pair[1]);
    }
    throw;
  }

#if defined(__APPLE__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
  pid = fork();
#if defined(__APPLE__)
#pragma clang diagnostic pop
#endif
  if (pid < 0)
  {
    const int error_number = errno;
    close_fd_if_open(readiness_pipe[0]);
    close_fd_if_open(readiness_pipe[1]);
    for (const auto &pair : control_channels)
    {
      close_fd_if_open(pair[0]);
      close_fd_if_open(pair[1]);
    }
    throw std::runtime_error(
        std::string("worker fork failed: ") + std::strerror(error_number));
  }

  if (pid == 0)
  {
    rb_thread_atfork();
    close_fd_if_open(inherited_control_fd);
    close_fd_if_open(readiness_pipe[0]);
    std::vector<int> child_control_channels;
    child_control_channels.reserve(control_channels.size());
    for (const auto &pair : control_channels)
    {
      close_fd_if_open(pair[0]);
      child_control_channels.push_back(pair[1]);
    }
    run_worker_process(
        std::move(child_control_channels),
        spawn_config.max_threads,
        spawn_config.port,
        spawn_config.max_request_head_bytes,
        spawn_config.max_request_body_bytes,
        readiness_pipe[1],
        static_cast<int>(worker_index),
        spawn_config.worker_processes,
        -1,
        spawn_config.socket_queue_capacity,
        spawn_config.host,
        spawn_config.max_connections,
        spawn_config.max_keepalive_requests,
        spawn_config.request_timeout_seconds,
        spawn_config.request_head_timeout_seconds,
        spawn_config.first_data_timeout_seconds,
        spawn_config.request_body_timeout_seconds,
        spawn_config.persistent_timeout_seconds,
        spawn_config.worker_timeout_seconds,
        spawn_config.tls,
        spawn_config.tls_certificate,
        spawn_config.tls_private_key,
        spawn_config.tls_ca_certificate,
        spawn_config.tls_verify_mode,
        spawn_config.tls_min_version,
        spawn_config.alpn_protocols,
        spawn_config.http2,
        spawn_config.http2_max_concurrent_streams,
        spawn_config.http2_initial_window_size,
        spawn_config.http2_max_frame_size,
        spawn_config.http2_header_table_size,
        spawn_config.stats_path,
        spawn_config.metrics_endpoint,
        spawn_config.debug_logging);
  }

  parent_control_channels.clear();
  parent_control_channels.reserve(control_channels.size());
  for (const auto &pair : control_channels)
  {
    close_fd_if_open(pair[1]);
    parent_control_channels.push_back(pair[0]);
  }
  close_fd_if_open(readiness_pipe[1]);

  WorkerBootstrapReport report;
  try
  {
    report = read_worker_bootstrap_report(readiness_pipe[0], spawn_config.worker_timeout_seconds);
  }
  catch (...)
  {
    close_fd_if_open(readiness_pipe[0]);
    throw;
  }
  close_fd_if_open(readiness_pipe[0]);

  if (report.status == WorkerBootstrapStatus::failed)
  {
    failure_diagnostic = *report.diagnostic;
    return false;
  }

  return true;
}

bool Vajra::runtime::NativeRuntime::boot_replacement_worker(
    const std::shared_ptr<SharedWorkerState> &worker_state,
    const WorkerSpawnConfig &,
    pid_t &pid,
    std::vector<int> &parent_control_channels,
    BootDiagnostic &failure_diagnostic)
{
  int control_fd = -1;
  {
    const std::lock_guard<std::mutex> lock(server_mutex_);
    control_fd = worker_spawner_fd_;
  }
  if (control_fd < 0)
  {
    throw std::runtime_error("worker spawner is not available for replacement");
  }

  const auto command = static_cast<std::uint8_t>(WorkerSpawnerCommand::spawn);
  const std::uint32_t worker_index = static_cast<std::uint32_t>(worker_state->worker_index);
  try
  {
    write_all_or_throw(control_fd, &command, sizeof(command));
    write_all_or_throw(control_fd, &worker_index, sizeof(worker_index));
  }
  catch (...)
  {
    stop_worker_spawner();
    throw;
  }

  WorkerSpawnerResponseHeader header{};
  try
  {
    if (!read_exact_or_eof(control_fd, &header, sizeof(header)))
    {
      throw std::runtime_error("worker spawner closed before responding");
    }
  }
  catch (...)
  {
    stop_worker_spawner();
    throw;
  }

  pid = static_cast<pid_t>(header.pid);
  if (header.status == WorkerBootstrapStatus::failed)
  {
    failure_diagnostic = BootDiagnostic{
        read_string_payload(control_fd),
        read_string_payload(control_fd),
        read_string_payload(control_fd)};
    return false;
  }

  if (header.status != WorkerBootstrapStatus::ready)
  {
    throw std::runtime_error("worker spawner returned an unknown replacement status");
  }

  parent_control_channels = receive_file_descriptors_or_throw(control_fd, header.control_channel_count);
  {
    const std::lock_guard<std::mutex> lock(worker_state->control_channel_mutex);
    worker_state->control_channel_fds = parent_control_channels;
  }
  {
    const std::lock_guard<std::mutex> lock(worker_state->request_channel_mutex);
    worker_state->request_channel_fds = parent_control_channels;
    worker_state->channel_generation.fetch_add(1, std::memory_order_acq_rel);
  }
  worker_state->control_channel_count.store(parent_control_channels.size(), std::memory_order_release);
  worker_state->request_channel_count.store(parent_control_channels.size(), std::memory_order_release);
  worker_state->pid.store(pid, std::memory_order_release);
  prepare_worker_replacement(worker_state);
  worker_state->spawned_by_worker_spawner.store(true, std::memory_order_release);
  return true;
}

void Vajra::runtime::NativeRuntime::replace_worker(const std::shared_ptr<SharedWorkerState> &worker_state)
{
  const std::uint64_t attempt_number =
      worker_state->replacement_attempt_count.fetch_add(1, std::memory_order_acq_rel) + 1;
  if (attempt_number > recovery_policy_.replacement_failure_limit)
  {
    worker_state->replacement_failure_count.fetch_add(1, std::memory_order_acq_rel);
    worker_state->replacement_needed.store(false, std::memory_order_release);
    worker_state->available.store(false, std::memory_order_release);
    worker_state->terminal_replacement_failure.store(true, std::memory_order_release);
    mark_recovery_transition(worker_state, WorkerRecoveryState::terminal_failure);
    Vajra::runtime::mark_worker_replacement_counters(
        worker_state->worker_index,
        worker_state->replacement_attempt_count.load(std::memory_order_acquire),
        worker_state->replacement_success_count.load(std::memory_order_acquire),
        worker_state->replacement_failure_count.load(std::memory_order_acquire));
    Vajra::runtime::mark_worker_terminal_replacement_failure(worker_state->worker_index, true);
    if (debug_logging_.load(std::memory_order_acquire))
    {
      log_worker_lifecycle_event(
          "worker_replacement_failed",
          worker_state->worker_index,
          worker_state->pid.load(std::memory_order_acquire),
          worker_state->lifecycle_state.load(std::memory_order_acquire),
          worker_state->health_state.load(std::memory_order_acquire),
          worker_state->recovery_state.load(std::memory_order_acquire),
          false,
          worker_state->last_exit_classification.load(std::memory_order_acquire),
          true,
          false,
          static_cast<int>(attempt_number));
    }
    return;
  }

  if (debug_logging_.load(std::memory_order_acquire))
  {
    log_worker_lifecycle_event(
        "worker_replacement_started",
        worker_state->worker_index,
        worker_state->pid.load(std::memory_order_acquire),
        WorkerLifecycleState::booting,
        WorkerHealthState::healthy,
        WorkerRecoveryState::replacing,
        false,
        worker_state->last_exit_classification.load(std::memory_order_acquire),
        false,
        true,
        static_cast<int>(attempt_number));
  }

  pid_t pid = -1;
  std::vector<int> parent_control_channels;
  BootDiagnostic failure_diagnostic{"worker_replacement_failed", "boot", "worker replacement bootstrap failed"};
  bool ready = false;
  try
  {
    ready = boot_replacement_worker(worker_state, worker_spawn_config_, pid, parent_control_channels, failure_diagnostic);
  }
  catch (...)
  {
    worker_state->replacement_failure_count.fetch_add(1, std::memory_order_acq_rel);
    worker_state->replacement_needed.store(true, std::memory_order_release);
    mark_recovery_transition(worker_state, WorkerRecoveryState::rejoin_pending);
    throw;
  }

  if (!ready)
  {
    worker_state->replacement_failure_count.fetch_add(1, std::memory_order_acq_rel);
    worker_state->replacement_needed.store(true, std::memory_order_release);
    mark_recovery_transition(worker_state, WorkerRecoveryState::rejoin_pending);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
    {
    }
    observe_worker_exit(worker_state, status);
    throw std::runtime_error(
        "Ruby worker replacement boot failed (" + failure_diagnostic.code + "/" + failure_diagnostic.category + "): " +
        failure_diagnostic.message);
  }

  mark_worker_ready(worker_state);
  worker_state->replacement_success_count.fetch_add(1, std::memory_order_acq_rel);
  Vajra::runtime::mark_worker_replacement_counters(
      worker_state->worker_index,
      worker_state->replacement_attempt_count.load(std::memory_order_acquire),
      worker_state->replacement_success_count.load(std::memory_order_acquire),
      worker_state->replacement_failure_count.load(std::memory_order_acquire));

  if (debug_logging_.load(std::memory_order_acquire))
  {
    log_worker_lifecycle_event(
        "worker_replacement_ready",
        worker_state->worker_index,
        pid,
        WorkerLifecycleState::ready,
        WorkerHealthState::healthy,
        worker_state->recovery_state.load(std::memory_order_acquire),
        true,
        WorkerExitClassification::none,
        worker_state->terminal_replacement_failure.load(std::memory_order_acquire),
        false,
        static_cast<int>(attempt_number));
  }
}

void Vajra::runtime::NativeRuntime::replace_failed_workers(
    const std::vector<std::shared_ptr<SharedWorkerState>> &worker_states)
{
  bool runtime_stopping = false;
  {
    const std::lock_guard<std::mutex> lock(server_mutex_);
    runtime_stopping = runtime_shutdown_started_ || stop_requested_ || shutting_down != 0;
  }
  if (runtime_stopping)
  {
    return;
  }

  for (const auto &worker_state : worker_states)
  {
    if (!worker_has_exited(worker_state) ||
        !worker_state->replacement_needed.load(std::memory_order_acquire))
    {
      continue;
    }
    bool expected = false;
    if (!worker_state->replacement_scheduled.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
    {
      continue;
    }
    const std::lock_guard<std::mutex> lock(server_mutex_);
    pending_replacements_.push_back(worker_state);
  }
  worker_state_changed_.notify_all();
}

void Vajra::runtime::NativeRuntime::initiate_worker_recovery(const std::shared_ptr<SharedWorkerState> &worker_state)
{
  bool expected = false;
  if (!worker_state->recovery_requested.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
  {
    return;
  }

  worker_state->available.store(false, std::memory_order_release);
  worker_state->expected_shutdown.store(false, std::memory_order_release);
  worker_state->timeout_escalation_pending.store(true, std::memory_order_release);
  worker_state->replacement_needed.store(true, std::memory_order_release);
  worker_state->timeout_kill_deadline_nanoseconds.store(
      steady_clock_nanoseconds_after(kWorkerTimeoutGracePeriod),
      std::memory_order_release);
  worker_state->recovery_deadline_nanoseconds.store(
      steady_clock_nanoseconds() + health_policy_.drain_deadline_nanoseconds,
      std::memory_order_release);
  mark_recovery_transition(worker_state, WorkerRecoveryState::draining);
  mark_worker_stopping(worker_state);

  const pid_t pid = worker_state->pid.load(std::memory_order_acquire);
  if (pid > 0)
  {
    (void)signal_process_with_retry(pid, SIGINT, "failed to signal unhealthy worker recovery");
  }
  worker_state_changed_.notify_all();
}

void Vajra::runtime::NativeRuntime::maybe_recover_unhealthy_workers(
    const std::vector<std::shared_ptr<SharedWorkerState>> &worker_states)
{
  bool runtime_stopping = false;
  HealthPolicy health_policy;
  {
    const std::lock_guard<std::mutex> lock(server_mutex_);
    runtime_stopping = runtime_shutdown_started_ || stop_requested_ || shutting_down != 0;
    health_policy = health_policy_;
  }
  if (runtime_stopping)
  {
    return;
  }

  for (const auto &worker_state : worker_states)
  {
    if (worker_state->lifecycle_state.load(std::memory_order_acquire) != WorkerLifecycleState::ready)
    {
      continue;
    }

    const WorkerHealthState health_state = worker_state->health_state.load(std::memory_order_acquire);
    if (health_state == WorkerHealthState::overloaded)
    {
      const std::int64_t overload_started = worker_state->overload_started_nanoseconds.load(std::memory_order_acquire);
      if (overload_started == 0 ||
          (steady_clock_nanoseconds() - overload_started) < health_policy.overload_recovery_threshold_nanoseconds)
      {
        continue;
      }
    }
    else if (health_state != WorkerHealthState::suspect &&
             health_state != WorkerHealthState::wedged)
    {
      continue;
    }

    initiate_worker_recovery(worker_state);
  }
}

void Vajra::runtime::NativeRuntime::drain_pending_replacements()
{
  std::vector<std::shared_ptr<SharedWorkerState>> replacements;
  {
    const std::lock_guard<std::mutex> lock(server_mutex_);
    replacements.swap(pending_replacements_);
  }

  for (const auto &worker_state : replacements)
  {
    worker_state->replacement_scheduled.store(false, std::memory_order_release);
    if (!worker_has_exited(worker_state) ||
        !worker_state->replacement_needed.load(std::memory_order_acquire))
    {
      continue;
    }

    try
    {
      replace_worker(worker_state);
    }
    catch (const std::exception &error)
    {
      log_runtime_error(
          "failed to replace worker index=" + std::to_string(worker_state->worker_index) + ": " + error.what());
    }
  }
}

void Vajra::runtime::NativeRuntime::clear_worker_runtime()
{
  std::vector<std::shared_ptr<SharedWorkerState>> worker_states;
  {
    const std::lock_guard<std::mutex> lock(server_mutex_);
    worker_states = std::move(worker_states_);
    worker_states_.clear();
    pending_replacements_.clear();
    stop_requested_ = false;
    worker_startup_in_progress_ = false;
    health_policy_ = HealthPolicy{};
    worker_spawn_config_ = WorkerSpawnConfig{};
    recovery_policy_ = RecoveryPolicy{};
    debug_logging_.store(false, std::memory_order_release);
  }

  configure_runtime_tracing(false, "", "", false, "", "tracecontext,baggage");
  set_runtime_tracing_available(false);

  for (const auto &worker_state : worker_states)
  {
    close_worker_control_channels(worker_state);
  }

  stop_worker_spawner();
  stop_worker_exit_watcher();
  if (runtime_state_ != nullptr)
  {
    release_runtime_state(runtime_state_);
    runtime_state_ = nullptr;
  }
}

void Vajra::runtime::NativeRuntime::install_server_instance(std::shared_ptr<Vajra::Server> server)
{
  const std::lock_guard<std::mutex> lock(server_mutex_);
  server_instance_ = std::move(server);
  worker_startup_in_progress_ = false;
}

std::shared_ptr<Vajra::Server> Vajra::runtime::NativeRuntime::take_server_instance()
{
  const std::lock_guard<std::mutex> lock(server_mutex_);
  std::shared_ptr<Vajra::Server> server = server_instance_;
  server_instance_.reset();
  return server;
}

bool Vajra::runtime::NativeRuntime::stop_worker_processes()
{
  std::vector<std::shared_ptr<SharedWorkerState>> worker_states;
  std::vector<bool> mark_expected_shutdown_after_signal;
  bool startup_in_progress = false;
  {
    const std::lock_guard<std::mutex> lock(server_mutex_);
    worker_states = worker_states_;
    startup_in_progress = worker_startup_in_progress_;
    if (!worker_states.empty() || startup_in_progress)
    {
      stop_requested_ = true;
    }
  }
  mark_expected_shutdown_after_signal.reserve(worker_states.size());

  for (const auto &worker_state : worker_states)
  {
    const WorkerLifecycleState current_state = worker_state->lifecycle_state.load(std::memory_order_acquire);
    mark_expected_shutdown_after_signal.push_back(
        current_state != WorkerLifecycleState::stopping &&
        current_state != WorkerLifecycleState::exited);
    mark_worker_stopping(worker_state);
    if (!worker_state->timeout_escalation_pending.load(std::memory_order_acquire))
    {
      close_worker_request_channels(worker_state);
      worker_state->timeout_kill_deadline_nanoseconds.store(0, std::memory_order_release);
    }
  }

  if (worker_states.empty())
  {
    return startup_in_progress;
  }

  for (std::size_t index = 0; index < worker_states.size(); ++index)
  {
    const auto &worker_state = worker_states[index];
    const pid_t pid = worker_state->pid.load(std::memory_order_acquire);
    if (pid <= 0)
    {
      continue;
    }
    try
    {
      if (signal_process_with_retry(pid, SIGINT, "failed to signal worker shutdown") &&
          mark_expected_shutdown_after_signal[index])
      {
        worker_state->expected_shutdown.store(true, std::memory_order_release);
      }
    }
    catch (const std::exception &error)
    {
      std::cerr << "[Vajra][error] " << utc_timestamp()
                << " failed to signal worker shutdown pid=" << pid
                << ": " << error.what()
                << std::endl;
    }
    catch (...)
    {
      std::cerr << "[Vajra][error] " << utc_timestamp()
                << " failed to signal worker shutdown pid=" << pid
                << ": unknown native error"
                << std::endl;
    }
  }

  return true;
}

void Vajra::runtime::NativeRuntime::replay_pending_stop_if_needed()
{
  bool should_stop = false;
  {
    const std::lock_guard<std::mutex> lock(server_mutex_);
    should_stop = stop_requested_ || shutting_down != 0;
  }

  if (should_stop)
  {
    stop_worker_processes();
  }
}

void Vajra::runtime::NativeRuntime::wait_for_worker_exit(const std::vector<std::shared_ptr<SharedWorkerState>> &worker_states)
{
  WorkerWaitContext context{this, &worker_states, ""};
  rb_thread_call_without_gvl(
      &NativeRuntime::wait_for_worker_exit_without_gvl,
      &context,
      RUBY_UBF_IO,
      nullptr);
  if (!context.error_message.empty())
  {
    throw std::runtime_error(context.error_message);
  }
}

void Vajra::runtime::NativeRuntime::wait_for_worker_exit_blocking(
    const std::vector<std::shared_ptr<SharedWorkerState>> &worker_states)
{
  {
    std::unique_lock<std::mutex> lock(server_mutex_);
    if (worker_exit_watcher_running_)
    {
      const auto shutdown_grace = std::chrono::seconds(std::max(1, worker_spawn_config_.worker_timeout_seconds));
      const auto all_workers_exited = [this, &worker_states]()
      {
        if (worker_exit_watcher_stop_requested_)
        {
          return true;
        }
        return std::all_of(worker_states.begin(), worker_states.end(), [](const auto &worker_state)
                           { return worker_has_exited(worker_state); });
      };
      worker_state_changed_.wait_for(lock, shutdown_grace, all_workers_exited);
      if (!std::all_of(worker_states.begin(), worker_states.end(), [](const auto &worker_state)
                       { return worker_has_exited(worker_state); }))
      {
        std::vector<pid_t> remaining_worker_pids;
        remaining_worker_pids.reserve(worker_states.size());
        for (const auto &worker_state : worker_states)
        {
          if (worker_has_exited(worker_state))
          {
            continue;
          }
          const pid_t pid = worker_state->pid.load(std::memory_order_acquire);
          if (pid > 0)
          {
            mark_recovery_transition(worker_state, WorkerRecoveryState::terminating);
            remaining_worker_pids.push_back(pid);
          }
        }

        lock.unlock();
        for (pid_t pid : remaining_worker_pids)
        {
          (void)signal_process_with_retry(pid, SIGKILL, "failed to force kill worker during shutdown");
        }
        lock.lock();
        worker_state_changed_.wait(lock, [this, &worker_states]()
                                   {
          if (worker_exit_watcher_stop_requested_)
          {
            return true;
          }
          return std::all_of(worker_states.begin(), worker_states.end(), [](const auto &worker_state) {
            return worker_has_exited(worker_state);
          }); });
        if (!std::all_of(worker_states.begin(), worker_states.end(), [](const auto &worker_state)
                         { return worker_has_exited(worker_state); }))
        {
          throw std::runtime_error("worker exit watcher stopped before all workers exited");
        }
      }
      return;
    }
  }

  for (const auto &worker_state : worker_states)
  {
    if (worker_has_exited(worker_state))
    {
      continue;
    }

    const pid_t pid = worker_state->pid.load(std::memory_order_acquire);
    if (pid <= 0)
    {
      continue;
    }

    if (worker_state->spawned_by_worker_spawner.load(std::memory_order_acquire))
    {
      while (process_is_alive(pid))
      {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      observe_worker_disappearance(worker_state);
      continue;
    }

    int status = 0;
    for (;;)
    {
      const pid_t wait_result = waitpid(pid, &status, 0);
      if (wait_result == pid)
      {
        observe_worker_exit(worker_state, status);
        break;
      }
      if (wait_result < 0 && errno == EINTR)
      {
        continue;
      }
      throw std::runtime_error("failed to wait for worker exit");
    }
  }
}

void *Vajra::runtime::NativeRuntime::wait_for_worker_exit_without_gvl(void *data)
{
  auto *context = static_cast<WorkerWaitContext *>(data);
  try
  {
    context->runtime->wait_for_worker_exit_blocking(*context->worker_states);
  }
  catch (const std::exception &error)
  {
    context->error_message = error.what();
  }
  catch (...)
  {
    context->error_message = "failed to wait for worker exit";
  }
  return nullptr;
}

void Vajra::runtime::NativeRuntime::observe_worker_exit(
    const std::shared_ptr<SharedWorkerState> &worker_state,
    int status)
{
  const WorkerLifecycleState current_state = worker_state->lifecycle_state.load(std::memory_order_acquire);
  const bool shutdown_expected = worker_state->expected_shutdown.load(std::memory_order_acquire);
  bool runtime_shutdown_requested = false;
  {
    const std::lock_guard<std::mutex> lock(server_mutex_);
    runtime_shutdown_requested = runtime_shutdown_started_ || stop_requested_ || shutting_down != 0;
  }

  WorkerExitClassification exit_classification = WorkerExitClassification::unexpected_exit;
  int exit_detail = 0;
  if (WIFEXITED(status))
  {
    exit_detail = WEXITSTATUS(status);
    if (current_state == WorkerLifecycleState::booting)
    {
      exit_classification = WorkerExitClassification::exit_before_ready;
    }
    else if (exit_detail == 0 &&
             (shutdown_expected ||
              (runtime_shutdown_requested && current_state == WorkerLifecycleState::stopping)))
    {
      exit_classification = WorkerExitClassification::expected_shutdown;
    }
    else
    {
      exit_classification = WorkerExitClassification::unexpected_status;
    }
  }
  else if (WIFSIGNALED(status))
  {
    exit_detail = WTERMSIG(status);
    if (current_state == WorkerLifecycleState::booting)
    {
      exit_classification = WorkerExitClassification::exit_before_ready;
    }
    else if (shutdown_expected)
    {
      exit_classification = WorkerExitClassification::expected_shutdown;
    }
    else
    {
      exit_classification = WorkerExitClassification::unexpected_signal;
    }
  }
  else if (current_state == WorkerLifecycleState::booting)
  {
    exit_classification = WorkerExitClassification::exit_before_ready;
  }

  log_unexpected_worker_exit(exit_classification, exit_detail);

  mark_worker_exit(worker_state, exit_classification, exit_detail);
  worker_state->pid.store(-1, std::memory_order_release);
}

void Vajra::runtime::NativeRuntime::observe_worker_disappearance(
    const std::shared_ptr<SharedWorkerState> &worker_state)
{
  const WorkerLifecycleState current_state = worker_state->lifecycle_state.load(std::memory_order_acquire);
  const bool shutdown_expected = worker_state->expected_shutdown.load(std::memory_order_acquire);
  bool runtime_shutdown_requested = false;
  {
    const std::lock_guard<std::mutex> lock(server_mutex_);
    runtime_shutdown_requested = runtime_shutdown_started_ || stop_requested_ || shutting_down != 0;
  }

  WorkerExitClassification exit_classification = WorkerExitClassification::unexpected_exit;
  if (current_state == WorkerLifecycleState::booting)
  {
    exit_classification = WorkerExitClassification::exit_before_ready;
  }
  else if (shutdown_expected ||
           (runtime_shutdown_requested && current_state == WorkerLifecycleState::stopping))
  {
    exit_classification = WorkerExitClassification::expected_shutdown;
  }

  log_unexpected_worker_exit(exit_classification, 0);
  mark_worker_exit(worker_state, exit_classification, 0);
  worker_state->pid.store(-1, std::memory_order_release);
}

void Vajra::runtime::NativeRuntime::refresh_worker_health(
    const std::vector<std::shared_ptr<SharedWorkerState>> &worker_states)
{
  HealthPolicy health_policy;
  {
    const std::lock_guard<std::mutex> lock(server_mutex_);
    health_policy = health_policy_;
  }

  const std::int64_t now_nanoseconds = steady_clock_nanoseconds();

  for (const auto &worker_state : worker_states)
  {
    const WorkerLifecycleState lifecycle_state = worker_state->lifecycle_state.load(std::memory_order_acquire);
    const WorkerRecoveryState recovery_state = worker_state->recovery_state.load(std::memory_order_acquire);
    WorkerHealthState next_state = WorkerHealthState::healthy;
    std::int64_t sampled_last_progress = 0;
    if (lifecycle_state == WorkerLifecycleState::stopping)
    {
      next_state = WorkerHealthState::degraded;
    }
    else if (lifecycle_state == WorkerLifecycleState::exited)
    {
      next_state = WorkerHealthState::wedged;
    }
    else if (worker_state->timeout_escalation_pending.load(std::memory_order_acquire))
    {
      next_state = WorkerHealthState::wedged;
    }
    else
    {
      const std::size_t active_executions = worker_state->active_execution_count.load(std::memory_order_acquire);
      const std::size_t idle_executions = worker_state->idle_execution_count.load(std::memory_order_acquire);
      const std::size_t total_executions = active_executions + idle_executions;
      const std::size_t local_queue_depth = worker_state->local_queue_depth.load(std::memory_order_acquire);
      const std::int64_t oldest_local_queue_age =
          worker_state->oldest_local_queue_age_nanoseconds.load(std::memory_order_acquire);
      sampled_last_progress = worker_state->last_progress_nanoseconds.load(std::memory_order_acquire);
      const std::int64_t progress_age = sampled_last_progress == 0 ? 0 : (now_nanoseconds - sampled_last_progress);
      const std::uint64_t unexpected_exits = worker_state->unexpected_exit_count.load(std::memory_order_acquire);
      const std::int64_t last_unexpected_exit =
          worker_state->last_unexpected_exit_nanoseconds.load(std::memory_order_acquire);
      const bool recovering =
          recovery_state == WorkerRecoveryState::draining ||
          recovery_state == WorkerRecoveryState::terminating ||
          recovery_state == WorkerRecoveryState::replacing ||
          recovery_state == WorkerRecoveryState::rejoin_pending;

      if (progress_age >= health_policy.wedged_threshold_nanoseconds && active_executions > 0)
      {
        next_state = WorkerHealthState::wedged;
      }
      else if (progress_age >= health_policy.suspect_threshold_nanoseconds && active_executions > 0)
      {
        next_state = WorkerHealthState::suspect;
      }
      else if (unexpected_exits > 0 &&
               last_unexpected_exit != 0 &&
               (now_nanoseconds - last_unexpected_exit) < health_policy.degraded_decay_nanoseconds)
      {
        next_state = WorkerHealthState::degraded;
      }
      else if (worker_state->terminal_replacement_failure.load(std::memory_order_acquire))
      {
        next_state = WorkerHealthState::wedged;
      }
      else if (recovering)
      {
        next_state = WorkerHealthState::degraded;
      }
      else if (total_executions > 0 &&
               active_executions >= total_executions &&
               local_queue_depth > 0 &&
               oldest_local_queue_age >= health_policy.overload_oldest_queue_age_nanoseconds)
      {
        next_state = WorkerHealthState::overloaded;
        std::int64_t overload_started = worker_state->overload_started_nanoseconds.load(std::memory_order_acquire);
        if (overload_started == 0)
        {
          worker_state->overload_started_nanoseconds.store(now_nanoseconds, std::memory_order_release);
        }
      }
      else if (total_executions > 0 && active_executions >= total_executions)
      {
        next_state = WorkerHealthState::busy;
      }
      else
      {
        next_state = WorkerHealthState::healthy;
        worker_state->overload_started_nanoseconds.store(0, std::memory_order_release);
      }

      if (unexpected_exits > 0 &&
          last_unexpected_exit != 0 &&
          (now_nanoseconds - last_unexpected_exit) >= health_policy.degraded_decay_nanoseconds)
      {
        worker_state->unexpected_exit_count.store(0, std::memory_order_release);
        worker_state->last_unexpected_exit_nanoseconds.store(0, std::memory_order_release);
      }
    }

    const WorkerHealthState previous_state = worker_state->health_state.load(std::memory_order_acquire);
    if (previous_state != next_state)
    {
      if ((next_state == WorkerHealthState::suspect || next_state == WorkerHealthState::wedged) &&
          lifecycle_state == WorkerLifecycleState::ready &&
          !worker_state->timeout_escalation_pending.load(std::memory_order_acquire))
      {
        const std::int64_t latest_progress = worker_state->last_progress_nanoseconds.load(std::memory_order_acquire);
        const std::size_t latest_active_executions = worker_state->active_execution_count.load(std::memory_order_acquire);
        const std::int64_t latest_progress_age = latest_progress == 0 ? 0 : (now_nanoseconds - latest_progress);
        if (latest_progress != 0 &&
            latest_progress != sampled_last_progress &&
            latest_progress_age < health_policy.suspect_threshold_nanoseconds)
        {
          next_state = latest_active_executions > 0 ? WorkerHealthState::busy : WorkerHealthState::healthy;
        }
      }

      if (next_state != WorkerHealthState::overloaded)
      {
        worker_state->overload_started_nanoseconds.store(0, std::memory_order_release);
      }

      worker_state->health_state.store(next_state, std::memory_order_release);
      worker_state->last_health_transition_nanoseconds.store(now_nanoseconds, std::memory_order_release);
      worker_state->health_transition_count.fetch_add(1, std::memory_order_acq_rel);
      if (health_requires_quarantine(next_state))
      {
        worker_state->available.store(false, std::memory_order_release);
      }
      else if (lifecycle_state == WorkerLifecycleState::ready)
      {
        worker_state->available.store(true, std::memory_order_release);
      }
      Vajra::runtime::mark_worker_health(worker_state->worker_index, next_state);
      Vajra::runtime::mark_worker_available(
          worker_state->worker_index,
          worker_state->available.load(std::memory_order_acquire));

      if (debug_logging_.load(std::memory_order_acquire))
      {
        log_worker_lifecycle_event(
            "worker_health_changed",
            worker_state->worker_index,
            worker_state->pid.load(std::memory_order_acquire),
            lifecycle_state,
            next_state,
            worker_state->recovery_state.load(std::memory_order_acquire),
            worker_state->available.load(std::memory_order_acquire),
            worker_state->last_exit_classification.load(std::memory_order_acquire),
            worker_state->terminal_replacement_failure.load(std::memory_order_acquire),
            worker_state->replacement_needed.load(std::memory_order_acquire),
            0);
      }
    }
  }
}

void Vajra::runtime::NativeRuntime::handle_worker_timeout(const std::shared_ptr<SharedWorkerState> &worker_state)
{
  const WorkerLifecycleState current_state = worker_state->lifecycle_state.load(std::memory_order_acquire);
  if (current_state == WorkerLifecycleState::exited)
  {
    return;
  }

  bool timeout_handling_started = false;
  if (!worker_state->timeout_handling_started.compare_exchange_strong(
          timeout_handling_started,
          true,
          std::memory_order_acq_rel,
          std::memory_order_acquire))
  {
    return;
  }

  worker_state->available.store(false, std::memory_order_release);
  worker_state->expected_shutdown.store(false, std::memory_order_release);
  worker_state->timeout_escalation_pending.store(true, std::memory_order_release);
  worker_state->timeout_escalation_count.fetch_add(1, std::memory_order_acq_rel);
  worker_state->timeout_kill_deadline_nanoseconds.store(
      steady_clock_nanoseconds_after(kWorkerTimeoutGracePeriod),
      std::memory_order_release);
  mark_worker_stopping(worker_state);

  const pid_t pid = worker_state->pid.load(std::memory_order_acquire);
  if (pid > 0)
  {
    (void)signal_process_with_retry(pid, SIGINT, "failed to signal timed out worker");
  }
  Vajra::runtime::mark_worker_timeout_escalations(
      worker_state->worker_index,
      worker_state->timeout_escalation_count.load(std::memory_order_acquire));
  worker_state_changed_.notify_all();
}

void Vajra::runtime::NativeRuntime::maybe_escalate_timed_out_workers(
    const std::vector<std::shared_ptr<SharedWorkerState>> &worker_states)
{
  const std::int64_t now_nanoseconds = steady_clock_nanoseconds();
  for (const auto &worker_state : worker_states)
  {
    if (!worker_state->timeout_escalation_pending.load(std::memory_order_acquire))
    {
      continue;
    }

    const std::int64_t kill_deadline = worker_state->timeout_kill_deadline_nanoseconds.load(std::memory_order_acquire);
    if (kill_deadline == 0 || kill_deadline > now_nanoseconds)
    {
      continue;
    }

    const pid_t pid = worker_state->pid.load(std::memory_order_acquire);
    if (pid > 0)
    {
      mark_recovery_transition(worker_state, WorkerRecoveryState::terminating);
      (void)signal_process_with_retry(pid, SIGKILL, "failed to force kill timed out worker");
    }
    worker_state->timeout_escalation_pending.store(false, std::memory_order_release);
    worker_state->timeout_kill_deadline_nanoseconds.store(0, std::memory_order_release);
  }
}

void Vajra::runtime::NativeRuntime::ensure_worker_exit_watcher_started()
{
  std::lock_guard<std::mutex> lock(server_mutex_);
  if (worker_exit_watcher_running_)
  {
    return;
  }

  worker_exit_watcher_stop_requested_ = false;
  worker_exit_watcher_running_ = true;
  worker_exit_watcher_ = std::thread([this]()
                                     { watch_worker_exits(); });
}

void Vajra::runtime::NativeRuntime::stop_worker_exit_watcher()
{
  {
    const std::lock_guard<std::mutex> lock(server_mutex_);
    if (!worker_exit_watcher_running_)
    {
      return;
    }
    worker_exit_watcher_stop_requested_ = true;
  }
  worker_state_changed_.notify_all();
  if (worker_exit_watcher_.joinable())
  {
    worker_exit_watcher_.join();
  }
  const std::lock_guard<std::mutex> lock(server_mutex_);
  worker_exit_watcher_running_ = false;
}

void Vajra::runtime::NativeRuntime::watch_worker_exits()
{
  try
  {
    for (;;)
    {
      std::vector<std::shared_ptr<SharedWorkerState>> worker_states;
      bool stop_requested = false;
      {
        const std::lock_guard<std::mutex> lock(server_mutex_);
        worker_states = worker_states_;
        stop_requested = worker_exit_watcher_stop_requested_;
      }

      maybe_escalate_timed_out_workers(worker_states);
      refresh_worker_health(worker_states);
      maybe_recover_unhealthy_workers(worker_states);

      bool observed_exit = false;
      bool any_live_workers = false;
      for (const auto &worker_state : worker_states)
      {
        if (worker_has_exited(worker_state))
        {
          continue;
        }

        const pid_t pid = worker_state->pid.load(std::memory_order_acquire);
        if (pid <= 0)
        {
          continue;
        }

        any_live_workers = true;
      }

      if (stop_requested && !any_live_workers)
      {
        return;
      }

      for (const auto &worker_state : worker_states)
      {
        if (worker_has_exited(worker_state))
        {
          continue;
        }

        const pid_t pid = worker_state->pid.load(std::memory_order_acquire);
        if (pid <= 0)
        {
          continue;
        }

        if (worker_state->spawned_by_worker_spawner.load(std::memory_order_acquire))
        {
          if (!process_is_alive(pid))
          {
            observe_worker_disappearance(worker_state);
            observed_exit = true;
          }
          continue;
        }

        int status = 0;
        for (;;)
        {
          const pid_t wait_result = waitpid(pid, &status, WNOHANG);
          if (wait_result == 0)
          {
            break;
          }
          if (wait_result == pid)
          {
            observe_worker_exit(worker_state, status);
            observed_exit = true;
            break;
          }
          if (wait_result < 0 && errno == EINTR)
          {
            continue;
          }
          if (wait_result < 0 && errno == ECHILD)
          {
            break;
          }
          throw std::runtime_error("failed to wait for worker exit");
        }
      }

      replace_failed_workers(worker_states);

      if (!observed_exit)
      {
        std::unique_lock<std::mutex> lock(server_mutex_);
        worker_state_changed_.wait_for(lock, watcher_sleep_interval(worker_states));
      }
    }
  }
  catch (const std::exception &error)
  {
    {
      const std::lock_guard<std::mutex> lock(server_mutex_);
      worker_exit_watcher_stop_requested_ = true;
      stop_requested_ = true;
    }
    std::cerr << "[Vajra][error] " << utc_timestamp()
              << " worker exit watcher failed: " << error.what()
              << std::endl;
    worker_state_changed_.notify_all();
  }
  catch (...)
  {
    {
      const std::lock_guard<std::mutex> lock(server_mutex_);
      worker_exit_watcher_stop_requested_ = true;
      stop_requested_ = true;
    }
    std::cerr << "[Vajra][error] " << utc_timestamp()
              << " worker exit watcher failed with an unknown native error"
              << std::endl;
    worker_state_changed_.notify_all();
  }
}

void Vajra::runtime::NativeRuntime::run_worker_process(
    std::vector<int> control_channel_fds,
    std::size_t max_threads,
    int port,
    std::size_t max_request_head_bytes,
    std::size_t max_request_body_bytes,
    int readiness_write_fd,
    int worker_index,
    int worker_processes,
    int inherited_listener_fd,
    std::size_t socket_queue_capacity,
    std::string host,
    std::size_t max_connections,
    std::size_t max_keepalive_requests,
    std::size_t request_timeout_seconds,
    int request_head_timeout_seconds,
    int first_data_timeout_seconds,
    int request_body_timeout_seconds,
    int persistent_timeout_seconds,
    int worker_timeout_seconds,
    bool tls,
    std::string tls_certificate,
    std::string tls_private_key,
    std::string tls_ca_certificate,
    std::string tls_verify_mode,
    std::string tls_min_version,
    std::vector<std::string> alpn_protocols,
    bool http2,
    std::size_t http2_max_concurrent_streams,
    std::size_t http2_initial_window_size,
    std::size_t http2_max_frame_size,
    std::size_t http2_header_table_size,
    std::string stats_path,
    std::string metrics_endpoint,
    bool debug_logging)
{
  (void)host;
  (void)http2_max_concurrent_streams;
  (void)http2_initial_window_size;
  (void)http2_max_frame_size;
  (void)http2_header_table_size;

  try
  {
    if (runtime_state_ != nullptr)
    {
      Vajra::runtime::install_worker_runtime_state(runtime_state_, static_cast<std::size_t>(worker_index), getpid());
    }

    const auto boot_started_at = std::chrono::steady_clock::now();
    const BootContractResult boot_result = BootContract::run(
        BootContractConfig{port, max_request_head_bytes, kWorkerBootstrapRuntimeRole});
    if (boot_result.status != BootStatus::ready)
    {
      exit_worker_bootstrap_failure(
          readiness_write_fd,
          BootContract::diagnostic_for_failure(boot_result),
          1);
    }
    Vajra::runtime::start_runtime_logging_worker();
    Vajra::runtime::start_runtime_tracing_worker();
    Vajra::rack::ensure_same_process_rack_execution_threads_started();

    auto rack_executor = std::make_shared<Vajra::rack::RackRequestExecutor>(
        std::shared_ptr<const Vajra::rack::RackExecutionTransport>{},
        Vajra::rack::ControlPlaneConfig{std::move(stats_path), std::move(metrics_endpoint)});

    Vajra::request::Http2Config worker_http2_config{
        http2_max_concurrent_streams,
        http2_initial_window_size,
        http2_max_frame_size,
        http2_header_table_size,
        max_request_head_bytes,
        max_request_body_bytes,
        max_keepalive_requests,
        socket_queue_capacity};

    Vajra::request::RequestProcessor request_processor(
        max_request_head_bytes,
        max_request_body_bytes,
        request_head_timeout_seconds,
        first_data_timeout_seconds,
        request_body_timeout_seconds,
        persistent_timeout_seconds,
        max_keepalive_requests,
        rack_executor,
        max_threads,
        http2,
        worker_http2_config);
    std::unique_ptr<Vajra::transport::TlsContext> tls_context;
    if (tls)
    {
      tls_context = std::make_unique<Vajra::transport::TlsContext>(Vajra::transport::TlsConfig{
          std::move(tls_certificate),
          std::move(tls_private_key),
          std::move(tls_ca_certificate),
          std::move(tls_verify_mode),
          std::move(tls_min_version),
          std::move(alpn_protocols),
          request_head_timeout_seconds,
          first_data_timeout_seconds,
          static_cast<int>(request_timeout_seconds)});
    }

    if (control_channel_fds.empty())
    {
      throw std::runtime_error("worker requires a control channel");
    }

    const std::size_t max_worker_connections = checked_add(
        max_threads,
        socket_queue_capacity,
        "worker connection capacity is too large");
    (void)max_connections;

    const std::size_t hardware_threads = std::max<unsigned int>(1, std::thread::hardware_concurrency());
    const std::size_t connection_thread_target = std::max<std::size_t>(
        max_threads,
        std::min<std::size_t>(max_connections, hardware_threads * 2));
    const std::size_t native_connection_loop_count = std::max<std::size_t>(
        1,
        std::min(max_worker_connections, connection_thread_target));

    std::vector<std::unique_ptr<WorkerHotPathLoopContext>> loop_contexts;
    loop_contexts.reserve(native_connection_loop_count);
    std::vector<std::thread> native_io_threads;
    native_io_threads.reserve(native_connection_loop_count + 2);
    std::unique_ptr<WorkerConnectionQueueState> connection_queue_state;
    std::unique_ptr<WorkerHotPathLoopContext> intake_context;
    std::unique_ptr<WorkerHotPathLoopContext> reactor_context;

    {
      connection_queue_state = std::make_unique<WorkerConnectionQueueState>();
      connection_queue_state->max_pending_connections = max_worker_connections;
      connection_queue_state->max_active_connections = native_connection_loop_count;
      connection_queue_state->reactor_fd = create_worker_reactor_fd();
      if (connection_queue_state->reactor_fd < 0)
      {
        throw std::runtime_error("worker failed to create native connection reactor");
      }
      connection_queue_state->capacity_gated = false;
      intake_context = std::make_unique<WorkerHotPathLoopContext>();
      intake_context->listener_fd = inherited_listener_fd;
      intake_context->control_fd = control_channel_fds.empty() ? -1 : control_channel_fds.front();
      intake_context->worker_index = worker_index;
      intake_context->connection_queue_state = connection_queue_state.get();
      native_io_threads.emplace_back(run_native_worker_connection_receiver, intake_context.get());
      reactor_context = std::make_unique<WorkerHotPathLoopContext>();
      reactor_context->worker_index = worker_index;
      reactor_context->connection_queue_state = connection_queue_state.get();
      reactor_context->tls_context = tls_context.get();
      reactor_context->request_head_timeout_seconds = request_head_timeout_seconds;
      reactor_context->first_data_timeout_seconds = first_data_timeout_seconds;
      reactor_context->persistent_timeout_seconds = persistent_timeout_seconds;
      native_io_threads.emplace_back(run_native_worker_connection_reactor, reactor_context.get());

      for (std::size_t thread_index = 0; thread_index < native_connection_loop_count; ++thread_index)
      {
        auto loop_context = std::make_unique<WorkerHotPathLoopContext>();
        loop_context->request_processor = &request_processor;
        loop_context->listener_fd = inherited_listener_fd;
        loop_context->control_fd = control_channel_fds.empty() ? -1 : control_channel_fds.front();
        loop_context->fallback_port = port;
        loop_context->worker_index = worker_index;
        loop_context->connection_queue_state = connection_queue_state.get();
        loop_context->tls_context = tls_context.get();
        loop_context->http2_enabled = http2;
        loop_context->http2_config = worker_http2_config;
        loop_context->request_executor = rack_executor;
        loop_context->request_head_timeout_seconds = request_head_timeout_seconds;
        loop_context->first_data_timeout_seconds = first_data_timeout_seconds;
        loop_context->persistent_timeout_seconds = persistent_timeout_seconds;
        loop_context->max_keepalive_requests = max_keepalive_requests;
        native_io_threads.emplace_back(run_native_worker_connection_loop, loop_context.get());
        loop_contexts.push_back(std::move(loop_context));
      }
    }

    report_worker_boot_ready(readiness_write_fd);
    if (debug_logging)
    {
      log_worker_bootstrap_ready(port, boot_result.runtime_role, worker_processes);
    }
    const auto boot_finished_at = std::chrono::steady_clock::now();
    const std::chrono::duration<double> boot_elapsed = boot_finished_at - boot_started_at;
    log_worker_booted(worker_index, getpid(), boot_elapsed.count());
    close(readiness_write_fd);

    while (!shutdown_requested_or_runtime_draining())
    {
      if (connection_queue_state != nullptr)
      {
        std::lock_guard<std::mutex> lock(connection_queue_state->mutex);
        if (connection_queue_state->shutdown_requested)
        {
          break;
        }
      }

      RuntimeSleepContext loop_sleep_context{std::chrono::milliseconds(50)};
      rb_thread_call_without_gvl(
          sleep_runtime_loop_without_gvl,
          &loop_sleep_context,
          RUBY_UBF_IO,
          nullptr);
    }

    close_fd_if_open(inherited_listener_fd);
    if (connection_queue_state)
    {
      std::deque<std::shared_ptr<WorkerConnectionState>> pending_connections;
      std::unordered_map<int, std::shared_ptr<WorkerConnectionState>> idle_connections;
      int reactor_fd = -1;
      {
        const std::lock_guard<std::mutex> lock(connection_queue_state->mutex);
        connection_queue_state->shutdown_requested = true;
        pending_connections.swap(connection_queue_state->pending_connections);
        idle_connections.swap(connection_queue_state->idle_connections);
        reactor_fd = connection_queue_state->reactor_fd;
        connection_queue_state->reactor_fd = -1;
      }
      connection_queue_state->condition.notify_all();
      for (auto &connection : pending_connections)
      {
        if (connection)
        {
          close_worker_connection_state_and_note(*connection);
        }
      }
      for (auto &entry : idle_connections)
      {
        if (entry.second)
        {
          close_worker_connection_state_and_note(*entry.second);
        }
      }
      close_fd_if_open(reactor_fd);
    }
    for (int control_channel_fd : control_channel_fds)
    {
      shutdown(control_channel_fd, SHUT_RDWR);
      close_fd_if_open(control_channel_fd);
    }

    const bool rack_execution_drained = wait_for_rack_execution_idle(
        std::chrono::seconds(std::max(1, worker_timeout_seconds)));
    if (rack_execution_drained && connection_queue_state)
    {
      const bool active_connections_drained = wait_for_active_connections_idle(
          *connection_queue_state,
          std::chrono::milliseconds(250));
      if (!active_connections_drained)
      {
        interrupt_active_worker_connections(*connection_queue_state);
      }
    }
    for (std::thread &native_io_thread : native_io_threads)
    {
      if (native_io_thread.joinable())
      {
        if (rack_execution_drained)
        {
          native_io_thread.join();
        }
        else
        {
          native_io_thread.detach();
        }
      }
    }
    Vajra::rack::shutdown_same_process_rack_execution_threads();
    Vajra::runtime::stop_runtime_tracing_worker();
    Vajra::runtime::stop_runtime_logging_worker();
    _exit(0);
  }
  catch (const std::exception &error)
  {
    exit_worker_bootstrap_failure(
        readiness_write_fd,
        BootDiagnostic{
            "worker_bootstrap_error",
            "boot",
            error.what()},
        1);
  }
}

void Vajra::runtime::NativeRuntime::run_master_dispatch_loop(
    int listener_fd,
    const RuntimeConfig &config,
    const std::vector<std::shared_ptr<SharedWorkerState>> &worker_states)
{
  const std::size_t per_worker_connection_limit = checked_add(
      config.max_threads,
      config.socket_queue_capacity,
      "worker connection capacity is too large");
  std::vector<CachedWorkerDispatchTarget> dispatch_targets;
  dispatch_targets.reserve(worker_states.size());
  for (const auto &worker_state : worker_states)
  {
    dispatch_targets.push_back(CachedWorkerDispatchTarget{worker_state, 0, -1});
  }
  std::size_t next_worker_cursor = 0;
  for (;;)
  {
    if (shutdown_requested_or_runtime_draining())
    {
      break;
    }

    pollfd listener_descriptor{listener_fd, POLLIN, 0};
    int poll_result = 0;
    poll_result = poll(&listener_descriptor, 1, 100);
    if (poll_result == 0)
    {
      continue;
    }
    if (poll_result < 0)
    {
      if (errno == EINTR)
      {
        continue;
      }
      throw std::runtime_error(std::string("master dispatch poll failed: ") + std::strerror(errno));
    }
    if ((listener_descriptor.revents & POLLIN) == 0)
    {
      continue;
    }

    const std::size_t worker_count = dispatch_targets.size();
    std::optional<std::size_t> first_candidate_index;
    for (std::size_t offset = 0; offset < worker_count; ++offset)
    {
      const std::size_t target_index = (next_worker_cursor + offset) % worker_count;
      const auto &worker_state = dispatch_targets[target_index].worker_state;
      if (!worker_state->available.load(std::memory_order_acquire))
      {
        continue;
      }
      if (worker_state->lifecycle_state.load(std::memory_order_acquire) != WorkerLifecycleState::ready)
      {
        continue;
      }

      const auto runtime_worker_state = &runtime_state_->workers[worker_state->worker_index];
      const std::size_t credits = worker_connection_credits(*runtime_worker_state, per_worker_connection_limit, 0);
      if (credits == 0)
      {
        continue;
      }
      first_candidate_index = target_index;
      break;
    }

    if (!first_candidate_index.has_value())
    {
      RuntimeSleepContext loop_sleep_context{std::chrono::milliseconds(1)};
      rb_thread_call_without_gvl(
          sleep_runtime_loop_without_gvl,
          &loop_sleep_context,
          RUBY_UBF_IO,
          nullptr);
      continue;
    }

    sockaddr_in client_addr{};
    socklen_t client_len = sizeof(client_addr);
    int client_fd = -1;
    client_fd = accept_client_cloexec(listener_fd, reinterpret_cast<sockaddr *>(&client_addr), &client_len);
    if (client_fd < 0)
    {
      if (errno == EINTR)
      {
        continue;
      }
      if (shutdown_requested_or_runtime_draining())
      {
        break;
      }
      throw std::runtime_error(std::string("master dispatch accept failed: ") + std::strerror(errno));
    }

    bool dispatched = false;
    for (std::size_t offset = 0; offset < worker_count; ++offset)
    {
      const std::size_t target_index = (*first_candidate_index + offset) % worker_count;
      CachedWorkerDispatchTarget &target = dispatch_targets[target_index];
      const auto &worker_state = target.worker_state;
      if (!worker_state->available.load(std::memory_order_acquire) ||
          worker_state->lifecycle_state.load(std::memory_order_acquire) != WorkerLifecycleState::ready)
      {
        continue;
      }

      const auto runtime_worker_state = &runtime_state_->workers[worker_state->worker_index];
      if (worker_connection_credits(*runtime_worker_state, per_worker_connection_limit, 0) == 0)
      {
        continue;
      }

      const std::uint64_t channel_generation = worker_state->channel_generation.load(std::memory_order_acquire);
      if (target.control_fd < 0 || target.control_channel_generation != channel_generation)
      {
        const std::lock_guard<std::mutex> lock(worker_state->control_channel_mutex);
        target.control_fd = worker_state->control_channel_fds.empty() ? -1 : worker_state->control_channel_fds.front();
        target.control_channel_generation = channel_generation;
      }
      if (target.control_fd < 0)
      {
        Vajra::runtime::note_master_fd_transfer_failure(worker_state->worker_index);
        continue;
      }

      try
      {
        send_single_file_descriptor_or_throw(target.control_fd, client_fd);
        Vajra::runtime::note_master_dispatch(worker_state->worker_index, 0);
        next_worker_cursor = (target_index + 1) % worker_count;
        dispatched = true;
        break;
      }
      catch (...)
      {
        Vajra::runtime::note_master_fd_transfer_failure(worker_state->worker_index);
      }
    }

    close_fd_if_open(client_fd);
    if (!dispatched)
    {
      continue;
    }
  }
}

void Vajra::runtime::NativeRuntime::start(const RuntimeConfig &config)
{
  SignalHandlerGuard signal_handler_guard;
  signal_handler_guard.install();

  shutting_down = 0;
  {
    std::lock_guard<std::mutex> lock(server_mutex_);
    runtime_shutdown_started_ = false;
  }

  try
  {
    if (!try_begin_startup())
    {
      std::cout << "Vajra already running" << std::endl;
      return;
    }

    if (!start_called_from_ruby_main_thread())
    {
      throw std::runtime_error("worker-only Vajra.start must be invoked from the Ruby main thread");
    }
    validate_worker_channel_capacity(config.workers);
    const bool debug_logging = debug_logging_enabled(config.log_level);
    {
      const std::lock_guard<std::mutex> lock(server_mutex_);
      health_policy_ = health_policy_for(config);
      worker_spawn_config_ = WorkerSpawnConfig{
          config.host,
          config.max_threads,
          config.port,
          config.max_connections,
          config.max_request_head_bytes,
          config.max_request_body_bytes,
          config.max_keepalive_requests,
          config.request_timeout_seconds,
          config.request_head_timeout_seconds,
          config.first_data_timeout_seconds,
          config.request_body_timeout_seconds,
          config.persistent_timeout_seconds,
          config.worker_timeout_seconds,
          config.tls,
          config.tls_certificate,
          config.tls_private_key,
          config.tls_ca_certificate,
          config.tls_verify_mode,
          config.tls_min_version,
          config.alpn_protocols,
          config.http2,
          config.http2_max_concurrent_streams,
          config.http2_initial_window_size,
          config.http2_max_frame_size,
          config.http2_header_table_size,
          config.workers,
          config.socket_queue_capacity,
          config.stats_path,
          config.metrics_endpoint,
          debug_logging};
      recovery_policy_ = RecoveryPolicy{kReplacementFailureLimit};
      debug_logging_.store(debug_logging, std::memory_order_release);
    }
    configure_runtime_logging(config.structured_logs, config.access_log, config.error_log, config.access_log_format);
    configure_runtime_tracing(
        config.trace_enabled,
        config.trace_endpoint,
        config.trace_service_name,
        config.trace_enabled && !config.trace_otel_owner,
        config.trace_resource_attributes,
        config.trace_propagators);
    const BootContractResult master_boot_result = BootContract::run(
        BootContractConfig{config.port, config.max_request_head_bytes, kMasterPreloadRuntimeRole});
    BootContract::ensure_ready(master_boot_result);

    listener::Socket listener_socket;
    listener::SocketBinding listener_binding = listener_socket.open(config.host, config.port, false);
    log_runtime_banner_start(config.host, listener_binding.port, config.workers, config.min_threads, config.max_threads);
    flush_runtime_logs();
    if (runtime_state_ != nullptr)
    {
      release_runtime_state(runtime_state_);
    }
    runtime_state_ = allocate_runtime_state();
    install_master_runtime_state(
        runtime_state_,
        static_cast<std::size_t>(config.workers),
        config.max_threads,
        config.socket_queue_capacity);
    set_runtime_listener_fd(listener_binding.fd);

    start_worker_spawner(worker_spawn_config_);

    std::vector<std::shared_ptr<SharedWorkerState>> booted_worker_states;

    for (int worker_index = 0; worker_index < config.workers; ++worker_index)
    {
      int readiness_pipe[2] = {-1, -1};
      if (pipe(readiness_pipe) != 0)
      {
        const int error_number = errno;
        throw std::runtime_error(
            std::string("worker bootstrap pipe creation failed: ") + std::strerror(error_number));
      }

      std::array<int, 2> control_channel = {-1, -1};
      if (socketpair(AF_UNIX, worker_control_socket_type(), 0, control_channel.data()) != 0)
      {
        const int error_number = errno;
        close_fd_if_open(readiness_pipe[0]);
        close_fd_if_open(readiness_pipe[1]);
        throw std::runtime_error(
            std::string("worker control channel creation failed: ") + std::strerror(error_number));
      }

#if defined(__APPLE__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
      const pid_t pid = fork();
#if defined(__APPLE__)
#pragma clang diagnostic pop
#endif
      if (pid < 0)
      {
        const int error_number = errno;
        close_fd_if_open(readiness_pipe[0]);
        close_fd_if_open(readiness_pipe[1]);
        close_fd_if_open(control_channel[0]);
        close_fd_if_open(control_channel[1]);
        throw std::runtime_error(
            std::string("worker fork failed: ") + std::strerror(error_number));
      }

      if (pid == 0)
      {
        rb_thread_atfork();
        notify_tracing_after_fork();
        close_fd_if_open(worker_spawner_fd_);
        close_fd_if_open(readiness_pipe[0]);
        close_fd_if_open(control_channel[0]);
        run_worker_process(
            {control_channel[1]},
            config.max_threads,
            listener_binding.port,
            config.max_request_head_bytes,
            config.max_request_body_bytes,
            readiness_pipe[1],
            worker_index,
            config.workers,
            listener_binding.fd,
            config.socket_queue_capacity,
            config.host,
            config.max_connections,
            config.max_keepalive_requests,
            config.request_timeout_seconds,
            config.request_head_timeout_seconds,
            config.first_data_timeout_seconds,
            config.request_body_timeout_seconds,
            config.persistent_timeout_seconds,
            config.worker_timeout_seconds,
            config.tls,
            config.tls_certificate,
            config.tls_private_key,
            config.tls_ca_certificate,
            config.tls_verify_mode,
            config.tls_min_version,
            config.alpn_protocols,
            config.http2,
            config.http2_max_concurrent_streams,
            config.http2_initial_window_size,
            config.http2_max_frame_size,
            config.http2_header_table_size,
            config.stats_path,
            config.metrics_endpoint,
            debug_logging);
      }

      const std::shared_ptr<SharedWorkerState> worker_state =
          register_worker_runtime(static_cast<std::size_t>(worker_index), pid, {control_channel[0]});
      close_fd_if_open(control_channel[1]);
      replay_pending_stop_if_needed();
      close_fd_if_open(readiness_pipe[1]);

      WorkerBootstrapReport report;
      try
      {
        report = read_worker_bootstrap_report(readiness_pipe[0], config.worker_timeout_seconds);
      }
      catch (...)
      {
        close_fd_if_open(readiness_pipe[0]);
        stop_worker_processes();
        wait_for_worker_exit(booted_worker_states);
        wait_for_worker_exit({worker_state});
        clear_worker_runtime();
        throw;
      }
      close_fd_if_open(readiness_pipe[0]);

      if (report.status == WorkerBootstrapStatus::failed)
      {
        stop_worker_processes();
        wait_for_worker_exit(booted_worker_states);
        wait_for_worker_exit({worker_state});
        clear_worker_runtime();
        const auto &diagnostic = report.diagnostic.value();
        throw std::runtime_error(
            "Ruby worker boot failed (" + diagnostic.code + "/" + diagnostic.category + "): " +
            diagnostic.message);
      }

      mark_worker_ready(worker_state);
      booted_worker_states.push_back(worker_state);
    }

    ensure_worker_exit_watcher_started();
    Vajra::runtime::start_runtime_logging_worker();
    std::atomic_bool master_loop_completed{false};
    std::mutex master_loop_mutex;
    std::string master_loop_error;
    std::thread master_loop_thread([this, config, booted_worker_states, listener_fd = listener_binding.fd, &master_loop_completed, &master_loop_mutex, &master_loop_error]() mutable
                                   {
      try
      {
        run_master_dispatch_loop(listener_fd, config, booted_worker_states);
      }
      catch (const std::exception &error)
      {
        const std::lock_guard<std::mutex> lock(master_loop_mutex);
        master_loop_error = error.what();
      }
      catch (...)
      {
        const std::lock_guard<std::mutex> lock(master_loop_mutex);
        master_loop_error = "master loop failed with an unknown native error";
      }
      master_loop_completed.store(true, std::memory_order_release);
      worker_state_changed_.notify_all(); });

    RuntimeSleepContext loop_sleep_context{std::chrono::milliseconds(50)};
    for (;;)
    {
      drain_pending_replacements();
      const bool all_workers_exited = std::all_of(
          booted_worker_states.begin(),
          booted_worker_states.end(),
          [](const auto &worker_state)
          { return worker_has_exited(worker_state); });
      bool shutdown_in_progress = false;
      {
        const std::lock_guard<std::mutex> lock(server_mutex_);
        shutdown_in_progress = runtime_shutdown_started_ || stop_requested_;
      }
      if (shutdown_requested() || shutdown_in_progress)
      {
        begin_runtime_shutdown();
        mark_runtime_shutdown_requested();
        (void)stop_worker_processes();
        close_fd_if_open(listener_binding.fd);
        set_runtime_listener_fd(-1);
      }
      if (master_loop_completed.load(std::memory_order_acquire))
      {
        break;
      }
      if ((shutdown_in_progress || shutting_down != 0) && all_workers_exited)
      {
        break;
      }

      rb_thread_call_without_gvl(
          sleep_runtime_loop_without_gvl,
          &loop_sleep_context,
          RUBY_UBF_IO,
          nullptr);
    }

    if (master_loop_thread.joinable())
    {
      master_loop_thread.join();
    }
    {
      const std::lock_guard<std::mutex> lock(master_loop_mutex);
      if (!master_loop_error.empty())
      {
        throw std::runtime_error(master_loop_error);
      }
    }

    begin_runtime_shutdown();
    mark_runtime_shutdown_requested();
    stop_worker_processes();
    wait_for_worker_exit(booted_worker_states);
    clear_worker_runtime();
    if (debug_logging)
    {
      log_runtime_stop_completed();
    }
    log_runtime_shutdown_complete();
    Vajra::runtime::stop_runtime_tracing_worker();
    Vajra::runtime::stop_runtime_logging_worker();
  }
  catch (...)
  {
    begin_runtime_shutdown();
    mark_runtime_shutdown_requested();
    stop_worker_processes();
    const std::vector<std::shared_ptr<SharedWorkerState>> live_worker_states = worker_states();
    if (!live_worker_states.empty())
    {
      wait_for_worker_exit(live_worker_states);
    }
    clear_worker_runtime();
    Vajra::runtime::stop_runtime_tracing_worker();
    Vajra::runtime::stop_runtime_logging_worker();
    throw;
  }
}

void Vajra::runtime::NativeRuntime::stop()
{
  const bool had_runtime = runtime_running();
  if (had_runtime)
  {
    begin_runtime_shutdown();
    mark_runtime_shutdown_requested();
    (void)stop_worker_processes();
  }
}

bool VajraNative::shutdown_requested()
{
  return Vajra::runtime::NativeRuntime::shutdown_requested();
}

void VajraNative::begin_runtime_shutdown()
{
  Vajra::runtime::NativeRuntime::instance().begin_runtime_shutdown();
}

void VajraNative::start(
    std::string host,
    int port,
    int workers,
    std::size_t min_threads,
    std::size_t max_threads,
    std::size_t max_connections,
    std::size_t socket_queue_capacity,
    std::size_t max_request_head_bytes,
    std::size_t max_request_body_bytes,
    std::size_t max_keepalive_requests,
    std::size_t request_timeout_seconds,
    int request_head_timeout_seconds,
    int first_data_timeout_seconds,
    int request_body_timeout_seconds,
    int persistent_timeout_seconds,
    int worker_timeout_seconds,
    bool tls,
    std::string tls_certificate,
    std::string tls_private_key,
    std::string tls_ca_certificate,
    std::string tls_verify_mode,
    std::string tls_min_version,
    std::vector<std::string> alpn_protocols,
    bool http2,
    std::size_t http2_max_concurrent_streams,
    std::size_t http2_initial_window_size,
    std::size_t http2_max_frame_size,
    std::size_t http2_header_table_size,
    std::string log_level,
    std::string access_log,
    std::string error_log,
    bool structured_logs,
    std::string access_log_format,
    std::string stats_path,
    std::string metrics_endpoint,
    bool trace_enabled,
    std::string trace_endpoint,
    std::string trace_service_name,
    bool trace_otel_owner,
    std::string trace_resource_attributes,
    std::string trace_propagators)
{
  Vajra::runtime::NativeRuntime::instance().start(Vajra::runtime::RuntimeConfig{
      std::move(host),
      port,
      workers,
      min_threads,
      max_threads,
      max_connections,
      socket_queue_capacity,
      max_request_head_bytes,
      max_request_body_bytes,
      max_keepalive_requests,
      request_timeout_seconds,
      request_head_timeout_seconds,
      first_data_timeout_seconds,
      request_body_timeout_seconds,
      persistent_timeout_seconds,
      worker_timeout_seconds,
      tls,
      std::move(tls_certificate),
      std::move(tls_private_key),
      std::move(tls_ca_certificate),
      std::move(tls_verify_mode),
      std::move(tls_min_version),
      std::move(alpn_protocols),
      http2,
      http2_max_concurrent_streams,
      http2_initial_window_size,
      http2_max_frame_size,
      http2_header_table_size,
      std::move(log_level),
      std::move(access_log),
      std::move(error_log),
      structured_logs,
      std::move(access_log_format),
      std::move(stats_path),
      std::move(metrics_endpoint),
      trace_enabled,
      std::move(trace_endpoint),
      std::move(trace_service_name),
      trace_otel_owner,
      std::move(trace_resource_attributes),
      std::move(trace_propagators)});
}

void VajraNative::stop()
{
  Vajra::runtime::NativeRuntime::instance().stop();
}
