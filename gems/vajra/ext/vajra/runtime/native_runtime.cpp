// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "runtime/native_runtime.hpp"

#include "listener/listener_socket.hpp"
#include "rack/rack_request_executor.hpp"
#include "request/request_processor.hpp"
#include "response/response_writer.hpp"
#include "ruby/thread.h"
#include "runtime/boot_contract.hpp"
#include "runtime/runtime_logging.hpp"
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
#include <signal.h>
#include <sstream>
#include <stdexcept>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <utility>
#include <unistd.h>

#if defined(__APPLE__)
#include <sys/event.h>
#endif

namespace
{
  volatile std::sig_atomic_t shutting_down = 0;
  constexpr const char *kMasterPreloadRuntimeRole = "ruby_master_preload";
  constexpr const char *kNativeRuntimeControlRole = "native_runtime_control";
  constexpr const char *kWorkerBootstrapRuntimeRole = "ruby_worker_bootstrap";
  constexpr const char *kMasterWorkerMode = "master_worker";
  constexpr std::size_t kMaxWorkerBootstrapStringPayloadBytes = 64 * 1024;
  constexpr auto kWorkerTimeoutGracePeriod = std::chrono::seconds(1);
  constexpr auto kWorkerExitWatcherIdlePollInterval = std::chrono::seconds(1);
  constexpr int kSignalRetryLimit = 5;
  constexpr std::uint64_t kReplacementFailureLimit = 3;

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

  WorkerBootstrapReport read_worker_bootstrap_report(int read_fd)
  {
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
  };

  struct CachedWorkerDispatchTarget
  {
    std::shared_ptr<Vajra::runtime::SharedWorkerState> worker_state;
    std::uint64_t control_channel_generation = 0;
    int control_fd = -1;
  };

  struct WorkerConnectionQueueState
  {
    std::mutex mutex;
    std::condition_variable condition;
    std::deque<int> pending_client_fds;
    std::size_t max_pending_connections = 0;
    std::size_t max_active_connections = 0;
    bool capacity_gated = false;
    bool shutdown_requested = false;
  };

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

  bool control_channel_shutdown_requested(int control_fd)
  {
    if (control_fd < 0)
    {
      return false;
    }

    pollfd control_poll{control_fd, POLLIN | POLLHUP | POLLERR, 0};
    const int poll_result = poll(&control_poll, 1, 0);
    if (poll_result <= 0)
    {
      return false;
    }
    if ((control_poll.revents & (POLLHUP | POLLERR)) != 0)
    {
      return true;
    }
    if ((control_poll.revents & POLLIN) == 0)
    {
      return false;
    }

    char buffer[16];
    const ssize_t bytes_read = recv(control_fd, buffer, sizeof(buffer), MSG_DONTWAIT);
    if (bytes_read == 0)
    {
      return true;
    }
    if (bytes_read < 0)
    {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
      {
        return false;
      }
      return errno != EINTR || shutdown_requested_or_runtime_draining();
    }

    return true;
  }

  int accept_client_connection(int listener_fd, int control_fd)
  {
    for (;;)
    {
      pollfd descriptors[2] = {
          {listener_fd, POLLIN, 0},
          {control_fd, POLLIN | POLLHUP | POLLERR, 0}};
      const nfds_t descriptor_count = control_fd >= 0 ? 2 : 1;
      const int poll_result = poll(descriptors, descriptor_count, 100);
      if (poll_result == 0)
      {
        if (shutdown_requested_or_runtime_draining())
        {
          return -1;
        }
        continue;
      }
      if (poll_result < 0)
      {
        if (errno == EINTR)
        {
          if (shutdown_requested_or_runtime_draining())
          {
            return -1;
          }
          continue;
        }
        throw std::runtime_error(std::string("worker accept poll failed: ") + std::strerror(errno));
      }
      if (control_fd >= 0 && (descriptors[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0)
      {
        return -1;
      }
      if ((descriptors[0].revents & POLLIN) == 0)
      {
        if (shutdown_requested_or_runtime_draining())
        {
          return -1;
        }
        continue;
      }

      const int client_fd = accept(listener_fd, nullptr, nullptr);
      if (client_fd >= 0)
      {
        return client_fd;
      }
      if (errno == EINTR)
      {
        if (shutdown_requested_or_runtime_draining())
        {
          return -1;
        }
        continue;
      }
      if (shutdown_requested_or_runtime_draining())
      {
        return -1;
      }
      throw std::runtime_error(std::string("worker accept failed: ") + std::strerror(errno));
    }
  }

  void set_nonblocking_or_throw(int fd)
  {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
    {
      throw std::runtime_error(std::string("fcntl(F_GETFL) failed: ") + std::strerror(errno));
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
    {
      throw std::runtime_error(std::string("fcntl(F_SETFL) failed: ") + std::strerror(errno));
    }
  }

  std::int64_t connection_deadline_nanoseconds(
      bool first_request,
      int first_data_timeout_seconds,
      int persistent_timeout_seconds)
  {
    const std::int64_t timeout_seconds = first_request ? first_data_timeout_seconds : persistent_timeout_seconds;
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch() + std::chrono::seconds(timeout_seconds))
        .count();
  }

  bool socket_has_pending_readable_bytes(int client_fd)
  {
    if (client_fd < 0)
    {
      return false;
    }

    pollfd descriptor{client_fd, POLLIN | POLLHUP | POLLERR, 0};
    const int poll_result = poll(&descriptor, 1, 0);
    if (poll_result <= 0)
    {
      return false;
    }

    return (descriptor.revents & (POLLIN | POLLHUP | POLLERR)) != 0;
  }

  bool enqueue_worker_connection(
      WorkerConnectionQueueState &queue_state,
      int client_fd,
      std::chrono::milliseconds wait_interval)
  {
    while (!shutdown_requested_or_runtime_draining())
    {
      std::unique_lock<std::mutex> lock(queue_state.mutex);
      if (queue_state.shutdown_requested)
      {
        return false;
      }
      const std::size_t pending_connections = queue_state.pending_client_fds.size();
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
        queue_state.pending_client_fds.push_back(client_fd);
        Vajra::runtime::note_worker_local_queue_depth(queue_state.pending_client_fds.size());
        lock.unlock();
        queue_state.condition.notify_one();
        return true;
      }
      queue_state.condition.wait_for(lock, wait_interval);
    }

    return false;
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
      std::optional<int> received_client_fd;
      try
      {
        received_client_fd = receive_single_file_descriptor(context->control_fd);
      }
      catch (const std::exception &error)
      {
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

      bool enqueued = false;
      enqueued = enqueue_worker_connection(queue_state, *received_client_fd, std::chrono::milliseconds(1));
      if (!enqueued)
      {
        close_fd_if_open(*received_client_fd);
        continue;
      }
    }

    return nullptr;
  }

  VALUE run_worker_connection_receiver(void *data)
  {
    rb_thread_lock_native_thread();
    rb_thread_call_without_gvl(
        run_worker_connection_receiver_without_gvl,
        data,
        RUBY_UBF_IO,
        nullptr);
    return Qnil;
  }

  std::optional<int> dequeue_worker_connection(WorkerConnectionQueueState &queue_state)
  {
    std::unique_lock<std::mutex> lock(queue_state.mutex);
    queue_state.condition.wait(lock, [&queue_state] {
      return queue_state.shutdown_requested || !queue_state.pending_client_fds.empty();
    });
    if (queue_state.pending_client_fds.empty())
    {
      return std::nullopt;
    }

    const int client_fd = queue_state.pending_client_fds.front();
    queue_state.pending_client_fds.pop_front();
    Vajra::runtime::note_worker_local_queue_depth(queue_state.pending_client_fds.size());
    lock.unlock();
    queue_state.condition.notify_one();
    return client_fd;
  }

  void handle_worker_connection(WorkerHotPathLoopContext &context, int client_fd)
  {
    Vajra::response::ResponseWriter::prepare_client_socket(client_fd);
    Vajra::runtime::note_worker_connection_opened();
    try
    {
      context.request_processor->handle(client_fd, socket_context_for_client_fd(client_fd, context.fallback_port));
    }
    catch (...)
    {
    }
    Vajra::runtime::note_worker_connection_closed();
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
      int client_fd = -1;
      try
      {
        if (context->connection_queue_state == nullptr)
        {
          break;
        }

        std::optional<int> dequeued_client_fd = dequeue_worker_connection(*context->connection_queue_state);
        if (!dequeued_client_fd.has_value())
        {
          break;
        }
        client_fd = *dequeued_client_fd;
        Vajra::runtime::note_worker_dispatch_received();
      }
      catch (const std::exception &error)
      {
        if (client_fd >= 0)
        {
          close_fd_if_open(client_fd);
        }
        Vajra::runtime::log_runtime_error(std::string("worker connection receive failed: ") + error.what());
        continue;
      }

      handle_worker_connection(*context, client_fd);
    }

    return nullptr;
  }

  VALUE run_worker_connection_loop(void *data)
  {
    rb_thread_lock_native_thread();
    rb_thread_call_without_gvl(
        run_worker_connection_loop_without_gvl,
        data,
        RUBY_UBF_IO,
        nullptr);
    return Qnil;
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
        readiness_pipe[1],
        static_cast<int>(worker_index),
        spawn_config.worker_processes,
        -1,
        spawn_config.socket_queue_capacity,
        spawn_config.host,
        spawn_config.max_connections,
        spawn_config.request_head_timeout_seconds,
        spawn_config.first_data_timeout_seconds,
        spawn_config.persistent_timeout_seconds,
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
    report = read_worker_bootstrap_report(readiness_pipe[0]);
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

  configure_runtime_tracing(false, "", "");
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
      worker_state_changed_.wait(lock, [this, &worker_states]() {
        if (worker_exit_watcher_stop_requested_)
        {
          return true;
        }
        return std::all_of(worker_states.begin(), worker_states.end(), [](const auto &worker_state) {
          return worker_has_exited(worker_state);
        });
      });
      if (!std::all_of(worker_states.begin(), worker_states.end(), [](const auto &worker_state) {
            return worker_has_exited(worker_state);
          }))
      {
        throw std::runtime_error("worker exit watcher stopped before all workers exited");
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
  worker_exit_watcher_ = std::thread([this]() { watch_worker_exits(); });
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
    int readiness_write_fd,
    int worker_index,
    int worker_processes,
    int inherited_listener_fd,
    std::size_t socket_queue_capacity,
    std::string host,
    std::size_t max_connections,
    int request_head_timeout_seconds,
    int first_data_timeout_seconds,
    int persistent_timeout_seconds,
    std::string stats_path,
    std::string metrics_endpoint,
    bool debug_logging)
{
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

    auto rack_executor = std::make_shared<Vajra::rack::RackRequestExecutor>(
        std::shared_ptr<const Vajra::rack::RackExecutionTransport>{},
        Vajra::rack::ControlPlaneConfig{std::move(stats_path), std::move(metrics_endpoint)});

    Vajra::request::RequestProcessor request_processor(
        max_request_head_bytes,
        request_head_timeout_seconds,
        first_data_timeout_seconds,
        persistent_timeout_seconds,
        std::move(rack_executor));

    if (control_channel_fds.empty())
    {
      throw std::runtime_error("worker requires a control channel");
    }

    const std::size_t max_worker_connections = checked_add(
        max_threads,
        socket_queue_capacity,
        "worker connection capacity is too large");
    (void)max_connections;

    std::vector<std::unique_ptr<WorkerHotPathLoopContext>> loop_contexts;
    loop_contexts.reserve(max_threads);
    std::unique_ptr<WorkerConnectionQueueState> connection_queue_state;
    std::unique_ptr<WorkerHotPathLoopContext> intake_context;

    {
      connection_queue_state = std::make_unique<WorkerConnectionQueueState>();
      connection_queue_state->max_pending_connections = max_worker_connections;
      connection_queue_state->max_active_connections = max_threads;
      connection_queue_state->capacity_gated = false;
      intake_context = std::make_unique<WorkerHotPathLoopContext>();
      intake_context->listener_fd = inherited_listener_fd;
      intake_context->control_fd = control_channel_fds.empty() ? -1 : control_channel_fds.front();
      intake_context->worker_index = worker_index;
      intake_context->connection_queue_state = connection_queue_state.get();
      rb_thread_create(run_worker_connection_receiver, intake_context.get());

      for (std::size_t thread_index = 0; thread_index < max_threads; ++thread_index)
      {
        auto loop_context = std::make_unique<WorkerHotPathLoopContext>();
        loop_context->request_processor = &request_processor;
        loop_context->listener_fd = inherited_listener_fd;
        loop_context->control_fd = control_channel_fds.empty() ? -1 : control_channel_fds.front();
        loop_context->fallback_port = port;
        loop_context->worker_index = worker_index;
        loop_context->connection_queue_state = connection_queue_state.get();
        rb_thread_create(run_worker_connection_loop, loop_context.get());
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
      if (control_channel_shutdown_requested(control_channel_fds.empty() ? -1 : control_channel_fds.front()))
      {
        break;
      }
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
      {
        const std::lock_guard<std::mutex> lock(connection_queue_state->mutex);
        connection_queue_state->shutdown_requested = true;
      }
      connection_queue_state->condition.notify_all();
    }
    for (int control_channel_fd : control_channel_fds)
    {
      shutdown(control_channel_fd, SHUT_RDWR);
      close_fd_if_open(control_channel_fd);
    }
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
    client_fd = accept(listener_fd, reinterpret_cast<sockaddr *>(&client_addr), &client_len);
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
          config.request_head_timeout_seconds,
          config.first_data_timeout_seconds,
          config.persistent_timeout_seconds,
          config.workers,
          config.socket_queue_capacity,
          config.stats_path,
          config.metrics_endpoint,
          debug_logging};
      recovery_policy_ = RecoveryPolicy{kReplacementFailureLimit};
      debug_logging_.store(debug_logging, std::memory_order_release);
    }
    configure_runtime_logging(config.structured_logs, config.access_log, config.error_log);
    configure_runtime_tracing(config.trace_enabled, config.trace_endpoint, config.trace_service_name);
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
        close_fd_if_open(worker_spawner_fd_);
        close_fd_if_open(readiness_pipe[0]);
        close_fd_if_open(control_channel[0]);
        run_worker_process(
            {control_channel[1]},
            config.max_threads,
            listener_binding.port,
            config.max_request_head_bytes,
            readiness_pipe[1],
            worker_index,
            config.workers,
            listener_binding.fd,
            config.socket_queue_capacity,
            config.host,
            config.max_connections,
            config.request_head_timeout_seconds,
            config.first_data_timeout_seconds,
            config.persistent_timeout_seconds,
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
        report = read_worker_bootstrap_report(readiness_pipe[0]);
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
    std::thread master_loop_thread([this, config, booted_worker_states, listener_fd = listener_binding.fd, &master_loop_completed, &master_loop_mutex, &master_loop_error]() mutable {
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
      worker_state_changed_.notify_all();
    });

    RuntimeSleepContext loop_sleep_context{std::chrono::milliseconds(50)};
    for (;;)
    {
      drain_pending_replacements();
      const bool all_workers_exited = std::all_of(
          booted_worker_states.begin(),
          booted_worker_states.end(),
          [](const auto &worker_state) { return worker_has_exited(worker_state); });
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
    std::size_t request_timeout_seconds,
    int request_head_timeout_seconds,
    int first_data_timeout_seconds,
    int persistent_timeout_seconds,
    int worker_timeout_seconds,
    std::string log_level,
    std::string access_log,
    std::string error_log,
    bool structured_logs,
    std::string stats_path,
    std::string metrics_endpoint,
    bool trace_enabled,
    std::string trace_endpoint,
    std::string trace_service_name)
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
      request_timeout_seconds,
      request_head_timeout_seconds,
      first_data_timeout_seconds,
      persistent_timeout_seconds,
      worker_timeout_seconds,
      std::move(log_level),
      std::move(access_log),
      std::move(error_log),
      structured_logs,
      std::move(stats_path),
      std::move(metrics_endpoint),
      trace_enabled,
      std::move(trace_endpoint),
      std::move(trace_service_name)});
}

void VajraNative::stop()
{
  Vajra::runtime::NativeRuntime::instance().stop();
}
