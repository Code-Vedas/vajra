// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "runtime/native_runtime.hpp"

#include "rack/rack_request_executor.hpp"
#include "ruby/thread.h"
#include "runtime/boot_contract.hpp"
#include "runtime/runtime_logging.hpp"
#include "vajra.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <limits>
#include <optional>
#include <signal.h>
#include <sstream>
#include <stdexcept>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

namespace
{
  volatile std::sig_atomic_t shutting_down = 0;
  constexpr const char *kMasterPreloadRuntimeRole = "ruby_master_preload";
  constexpr const char *kNativeRuntimeControlRole = "native_runtime_control";
  constexpr const char *kWorkerBootstrapRuntimeRole = "ruby_worker_bootstrap";
  constexpr const char *kMasterWorkerMode = "master_worker";
  constexpr std::size_t kMaxWorkerBootstrapStringPayloadBytes = 64 * 1024;

  enum class WorkerBootstrapStatus : std::uint8_t
  {
    ready = 1,
    failed = 2,
  };

  struct WorkerBootstrapReport
  {
    WorkerBootstrapStatus status;
    std::optional<Vajra::runtime::BootDiagnostic> diagnostic;
  };

  struct ServerRunContext
  {
    Vajra::Server *server;
    std::string error_message;
  };

  struct WorkerWaitContext
  {
    Vajra::runtime::NativeRuntime *runtime;
    std::vector<pid_t> pids;
    std::string error_message;
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

  void validate_worker_channel_capacity(int workers, std::size_t max_threads)
  {
    constexpr std::size_t kRuntimeFdReserve = 32;
    const std::size_t total_request_channels = checked_multiply(
        static_cast<std::size_t>(workers),
        max_threads,
        "invalid workers/threads combination: workers * max_threads is too large");
    const std::size_t boot_request_channel_fds = checked_multiply(
        max_threads,
        static_cast<std::size_t>(2),
        "invalid workers/threads combination: worker boot request channel fd count is too large");
    const std::size_t boot_readiness_pipe_fds = static_cast<std::size_t>(2);
    const std::size_t boot_overhead_fds = checked_add(
        boot_request_channel_fds,
        boot_readiness_pipe_fds,
        "invalid workers/threads combination: worker boot fd count is too large");
    const std::size_t peak_parent_fds = checked_add(
        total_request_channels,
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
          "invalid workers/threads combination: workers * max_threads would keep " +
          std::to_string(total_request_channels) + " parent request-channel fds open in steady state and peak at " +
          std::to_string(peak_parent_fds) + " parent fds during worker boot (" +
          std::to_string(boot_request_channel_fds) + " boot request-channel fds plus " +
          std::to_string(boot_readiness_pipe_fds) + " readiness-pipe fds), which exceeds the process fd limit of " +
          std::to_string(available_fds) + ". Lower workers or threads, or raise the fd limit.");
    }
  }

  void *run_server_without_gvl(void *data)
  {
    auto *context = static_cast<ServerRunContext *>(data);
    try
    {
      context->server->start();
    }
    catch (const std::exception &error)
    {
      context->error_message = error.what();
    }
    catch (...)
    {
      context->error_message = "server failed with an unknown native error";
    }

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
    _exit(exit_code);
  }

  void *wait_for_worker_exit_without_gvl(void *data)
  {
    auto *context = static_cast<WorkerWaitContext *>(data);
    bool forwarded_shutdown = false;
    for (;;)
    {
      bool any_remaining = false;
      for (auto iterator = context->pids.begin(); iterator != context->pids.end();)
      {
        int status = 0;
        const pid_t wait_result = waitpid(*iterator, &status, WNOHANG);
        if (wait_result == 0)
        {
          any_remaining = true;
          ++iterator;
          continue;
        }
        if (wait_result == *iterator)
        {
          if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0))
          {
            if (WIFEXITED(status))
            {
              context->error_message =
                  "worker process exited unexpectedly with status " + std::to_string(WEXITSTATUS(status));
            }
            else if (WIFSIGNALED(status))
            {
              context->error_message =
                  "worker process exited unexpectedly due to signal " + std::to_string(WTERMSIG(status));
            }
            else
            {
              context->error_message = "worker process exited unexpectedly";
            }
            return nullptr;
          }
          iterator = context->pids.erase(iterator);
          continue;
        }
        if (wait_result < 0 && errno == EINTR)
        {
          break;
        }

        context->error_message = "failed to wait for worker process";
        return nullptr;
      }

      if (context->pids.empty())
      {
        return nullptr;
      }

      if (Vajra::runtime::NativeRuntime::shutdown_requested() && !forwarded_shutdown)
      {
        context->runtime->forward_shutdown_to_workers();
        forwarded_shutdown = true;
      }

      if (any_remaining)
      {
        usleep(10'000);
        continue;
      }
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
  return !worker_pids_.empty() || server_instance_ || worker_startup_in_progress_;
}

bool Vajra::runtime::NativeRuntime::try_begin_startup()
{
  const std::lock_guard<std::mutex> lock(server_mutex_);
  if (!worker_pids_.empty() || server_instance_ || worker_startup_in_progress_)
  {
    return false;
  }

  worker_startup_in_progress_ = true;
  return true;
}

void Vajra::runtime::NativeRuntime::set_worker_runtime(pid_t pid, int request_channel_fd)
{
  const std::lock_guard<std::mutex> lock(server_mutex_);
  worker_pids_.push_back(pid);
  worker_request_channel_fds_.push_back(request_channel_fd);
  worker_startup_in_progress_ = false;
}

void Vajra::runtime::NativeRuntime::clear_worker_runtime()
{
  std::vector<int> request_channel_fds;
  {
    const std::lock_guard<std::mutex> lock(server_mutex_);
    request_channel_fds = std::move(worker_request_channel_fds_);
    worker_pids_.clear();
    worker_request_channel_fds_.clear();
    stop_requested_ = false;
    worker_startup_in_progress_ = false;
  }

  for (int request_channel_fd : request_channel_fds)
  {
    close_fd_if_open(request_channel_fd);
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
  std::vector<pid_t> pids;
  bool startup_in_progress = false;
  std::vector<int> request_channel_fds;
  {
    const std::lock_guard<std::mutex> lock(server_mutex_);
    pids = worker_pids_;
    request_channel_fds = worker_request_channel_fds_;
    startup_in_progress = worker_startup_in_progress_;
    if (!pids.empty() || startup_in_progress)
    {
      stop_requested_ = true;
    }
  }

  for (int request_channel_fd : request_channel_fds)
  {
    shutdown(request_channel_fd, SHUT_RDWR);
    close_fd_if_open(request_channel_fd);
  }
  {
    const std::lock_guard<std::mutex> lock(server_mutex_);
    worker_request_channel_fds_.clear();
  }

  if (pids.empty())
  {
    return startup_in_progress;
  }

  if (!request_channel_fds.empty())
  {
    return true;
  }

  for (pid_t pid : pids)
  {
    for (;;)
    {
      if (kill(pid, SIGINT) == 0 || errno == ESRCH)
      {
        break;
      }

      if (errno == EINTR)
      {
        continue;
      }

      throw std::runtime_error("failed to signal worker shutdown");
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

void Vajra::runtime::NativeRuntime::wait_for_worker_exit(const std::vector<pid_t> &pids)
{
  WorkerWaitContext context{this, pids, ""};
  rb_thread_call_without_gvl(
      wait_for_worker_exit_without_gvl,
      &context,
      RUBY_UBF_IO,
      nullptr);
  if (!context.error_message.empty())
  {
    throw std::runtime_error(context.error_message);
  }
}

void Vajra::runtime::NativeRuntime::reap_worker_process(pid_t pid) const
{
  for (;;)
  {
    int status = 0;
    const pid_t wait_result = waitpid(pid, &status, 0);
    if (wait_result == pid)
    {
      return;
    }

    if (wait_result < 0 && errno == EINTR)
    {
      continue;
    }

    return;
  }
}

void Vajra::runtime::NativeRuntime::reap_worker_processes(const std::vector<pid_t> &pids) const
{
  for (pid_t pid : pids)
  {
    reap_worker_process(pid);
  }
}

void Vajra::runtime::NativeRuntime::run_worker_process(
    std::vector<int> request_channel_fds,
    std::size_t max_threads,
    int port,
    std::size_t max_request_head_bytes,
    int readiness_write_fd,
    int worker_index,
    int worker_processes,
    bool debug_logging)
{
  try
  {
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

    report_worker_boot_ready(readiness_write_fd);
    if (debug_logging)
    {
      std::cout << "[Vajra][lifecycle] " << utc_timestamp()
                << " event=worker_ready state=booting boot_status=ready stop_reason=none"
                << " port=" << port
                << " listener_owned=false listener_fd=-1"
                << " mode=" << kMasterWorkerMode
                << " process_role=" << boot_result.runtime_role
                << " request_execution_role=" << boot_result.runtime_role
                << " worker_processes=" << worker_processes
                << std::endl;
    }
    const auto boot_finished_at = std::chrono::steady_clock::now();
    const std::chrono::duration<double> boot_elapsed = boot_finished_at - boot_started_at;
    log_worker_booted(worker_index, getpid(), boot_elapsed.count());
    close(readiness_write_fd);
    Vajra::rack::run_worker_request_execution_loop(request_channel_fds, max_threads);
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

void Vajra::runtime::NativeRuntime::run_master_runtime_server(
    const RuntimeConfig &config,
    const std::vector<std::vector<int>> &request_channel_fds,
    const std::vector<pid_t> &worker_pids_for_runtime,
    bool debug_logging)
{
  auto server = std::make_shared<Vajra::Server>(
      config.port,
      config.host,
      config.max_request_head_bytes,
      std::make_shared<Vajra::rack::RackRequestExecutor>(
          Vajra::rack::request_channel_transport(
              request_channel_fds,
              std::vector<int>(worker_pids_for_runtime.begin(), worker_pids_for_runtime.end()),
              config.min_threads,
              config.queue_capacity,
              config.request_timeout_seconds,
              config.worker_timeout_seconds,
              debug_logging)),
      kNativeRuntimeControlRole,
      kMasterWorkerMode,
      config.workers,
      kWorkerBootstrapRuntimeRole,
      debug_logging,
      -1,
      config.request_head_timeout_seconds,
      config.first_data_timeout_seconds,
      config.persistent_timeout_seconds,
      config.max_connections,
      [this]() { begin_runtime_shutdown(); });
  Vajra::Server *server_ptr = server.get();
  install_server_instance(std::move(server));
  ServerRunContext context{server_ptr, ""};

  try
  {
    rb_thread_call_without_gvl(
        run_server_without_gvl,
        &context,
        RUBY_UBF_IO,
        nullptr);
    if (!context.error_message.empty())
    {
      throw std::runtime_error(context.error_message);
    }
  }
  catch (...)
  {
    auto owned_server = take_server_instance();
    if (owned_server)
    {
      owned_server->stop();
    }
    throw;
  }

  auto owned_server = take_server_instance();
  if (owned_server)
  {
    owned_server->stop();
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
    if (config.scheduler_policy != "least_loaded")
    {
      throw std::runtime_error("unsupported scheduler_policy: " + config.scheduler_policy);
    }
    validate_worker_channel_capacity(config.workers, config.max_threads);
    const bool debug_logging = debug_logging_enabled(config.log_level);
    log_runtime_banner_start(config.host, config.port, config.workers, config.min_threads, config.max_threads);
    const BootContractResult master_boot_result = BootContract::run(
        BootContractConfig{config.port, config.max_request_head_bytes, kMasterPreloadRuntimeRole});
    BootContract::ensure_ready(master_boot_result);

    std::vector<pid_t> booted_worker_pids;
    std::vector<std::vector<int>> parent_request_channels;

    for (int worker_index = 0; worker_index < config.workers; ++worker_index)
    {
      int readiness_pipe[2] = {-1, -1};
      if (pipe(readiness_pipe) != 0)
      {
        const int error_number = errno;
        throw std::runtime_error(
            std::string("worker bootstrap pipe creation failed: ") + std::strerror(error_number));
      }

      std::vector<std::array<int, 2>> request_channels;
      request_channels.reserve(config.max_threads);
      for (std::size_t thread_index = 0; thread_index < config.max_threads; ++thread_index)
      {
        std::array<int, 2> request_channel = {-1, -1};
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, request_channel.data()) != 0)
        {
          const int error_number = errno;
          close_fd_if_open(readiness_pipe[0]);
          close_fd_if_open(readiness_pipe[1]);
          for (const auto &pair : request_channels)
          {
            close_fd_if_open(pair[0]);
            close_fd_if_open(pair[1]);
          }
          throw std::runtime_error(
              std::string("worker request channel creation failed: ") + std::strerror(error_number));
        }
        request_channels.push_back(request_channel);
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
        for (const auto &pair : request_channels)
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
        close_fd_if_open(readiness_pipe[0]);
        std::vector<int> child_request_channels;
        child_request_channels.reserve(request_channels.size());
        for (const auto &pair : request_channels)
        {
          close_fd_if_open(pair[0]);
          child_request_channels.push_back(pair[1]);
        }
        run_worker_process(
            std::move(child_request_channels),
            config.max_threads,
            config.port,
            config.max_request_head_bytes,
            readiness_pipe[1],
            worker_index,
            config.workers,
            debug_logging);
      }

      std::vector<int> worker_parent_channels;
      worker_parent_channels.reserve(request_channels.size());
      for (const auto &pair : request_channels)
      {
        close_fd_if_open(pair[1]);
        worker_parent_channels.push_back(pair[0]);
      }
      set_worker_runtime(pid, worker_parent_channels.front());
      {
        const std::lock_guard<std::mutex> lock(server_mutex_);
        for (std::size_t index = 1; index < worker_parent_channels.size(); ++index)
        {
          worker_request_channel_fds_.push_back(worker_parent_channels[index]);
        }
      }
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
        reap_worker_processes(booted_worker_pids);
        reap_worker_process(pid);
        clear_worker_runtime();
        throw;
      }
      close_fd_if_open(readiness_pipe[0]);

      if (report.status == WorkerBootstrapStatus::failed)
      {
        stop_worker_processes();
        reap_worker_processes(booted_worker_pids);
        reap_worker_process(pid);
        clear_worker_runtime();
        const auto &diagnostic = report.diagnostic.value();
        throw std::runtime_error(
            "Ruby worker boot failed (" + diagnostic.code + "/" + diagnostic.category + "): " +
            diagnostic.message);
      }

      booted_worker_pids.push_back(pid);
      parent_request_channels.push_back(std::move(worker_parent_channels));
    }

    run_master_runtime_server(config, parent_request_channels, booted_worker_pids, debug_logging);
    begin_runtime_shutdown();
    stop_worker_processes();
    wait_for_worker_exit(booted_worker_pids);
    clear_worker_runtime();
    log_runtime_shutdown_complete();
  }
  catch (...)
  {
    clear_worker_runtime();
    throw;
  }
}

void Vajra::runtime::NativeRuntime::stop()
{
  if (runtime_running())
  {
    begin_runtime_shutdown();
  }
  (void)stop_worker_processes();

  Vajra::Server *server = nullptr;
  std::shared_ptr<Vajra::Server> server_handle;
  {
    std::lock_guard<std::mutex> lock(server_mutex_);
    server_handle = server_instance_;
    server = server_handle.get();
  }

  if (server != nullptr)
  {
    server->stop();
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
    std::size_t queue_capacity,
    std::string scheduler_policy,
    std::size_t max_request_head_bytes,
    std::size_t request_timeout_seconds,
    int request_head_timeout_seconds,
    int first_data_timeout_seconds,
    int persistent_timeout_seconds,
    int worker_timeout_seconds,
    std::string log_level)
{
  Vajra::runtime::NativeRuntime::instance().start(Vajra::runtime::RuntimeConfig{
      std::move(host),
      port,
      workers,
      min_threads,
      max_threads,
      max_connections,
      queue_capacity,
      std::move(scheduler_policy),
      max_request_head_bytes,
      request_timeout_seconds,
      request_head_timeout_seconds,
      first_data_timeout_seconds,
      persistent_timeout_seconds,
      worker_timeout_seconds,
      std::move(log_level)});
}

void VajraNative::stop()
{
  Vajra::runtime::NativeRuntime::instance().stop();
}
