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
#include <cstdlib>
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
#include <utility>
#include <unistd.h>

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

  struct WorkerBootstrapReport
  {
    WorkerBootstrapStatus status;
    std::optional<Vajra::runtime::BootDiagnostic> diagnostic;
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

  bool worker_has_exited(const std::shared_ptr<Vajra::runtime::SharedWorkerState> &worker_state)
  {
    return worker_state->lifecycle_state.load(std::memory_order_acquire) == Vajra::runtime::WorkerLifecycleState::exited;
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
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::seconds(std::max(5, config.worker_timeout_seconds / 2)))
            .count(),
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::seconds(std::max(10, config.worker_timeout_seconds)))
            .count(),
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::seconds(std::max(30, config.worker_timeout_seconds * 2)))
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
    std::vector<int> request_channel_fds)
{
  const auto worker_state =
      std::make_shared<SharedWorkerState>(worker_index, pid, std::move(request_channel_fds));
  {
    const std::lock_guard<std::mutex> lock(server_mutex_);
    worker_states_.push_back(worker_state);
    worker_startup_in_progress_ = false;
  }
  worker_state->last_progress_nanoseconds.store(steady_clock_nanoseconds(), std::memory_order_release);
  worker_state->last_lifecycle_transition_nanoseconds.store(steady_clock_nanoseconds(), std::memory_order_release);
  worker_state->last_health_transition_nanoseconds.store(steady_clock_nanoseconds(), std::memory_order_release);
  worker_state->idle_execution_count.store(worker_state->request_channel_fds.size(), std::memory_order_release);
  if (debug_logging_.load(std::memory_order_acquire))
  {
    log_worker_lifecycle_event(
        "worker_registered",
        worker_state->worker_index,
        pid,
        WorkerLifecycleState::booting,
        worker_state->health_state.load(std::memory_order_acquire),
        false,
        WorkerExitClassification::none,
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
  mark_lifecycle_transition(worker_state, WorkerLifecycleState::ready);
  worker_state->health_state.store(WorkerHealthState::healthy, std::memory_order_release);
  worker_state->last_health_transition_nanoseconds.store(steady_clock_nanoseconds(), std::memory_order_release);
  if (debug_logging_.load(std::memory_order_acquire))
  {
    log_worker_lifecycle_event(
        "worker_ready",
        worker_state->worker_index,
        worker_state->pid.load(std::memory_order_acquire),
        WorkerLifecycleState::ready,
        worker_state->health_state.load(std::memory_order_acquire),
        true,
        WorkerExitClassification::none,
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
  worker_state->last_health_transition_nanoseconds.store(steady_clock_nanoseconds(), std::memory_order_release);
  if (debug_logging_.load(std::memory_order_acquire))
  {
    log_worker_lifecycle_event(
        "worker_stopping",
        worker_state->worker_index,
        worker_state->pid.load(std::memory_order_acquire),
        WorkerLifecycleState::stopping,
        worker_state->health_state.load(std::memory_order_acquire),
        false,
        worker_state->last_exit_classification.load(std::memory_order_acquire),
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
  close_worker_request_channels(worker_state);
  if (debug_logging_.load(std::memory_order_acquire))
  {
    log_worker_lifecycle_event(
        "worker_exited",
        worker_state->worker_index,
        worker_state->pid.load(std::memory_order_acquire),
        WorkerLifecycleState::exited,
        worker_state->health_state.load(std::memory_order_acquire),
        false,
        exit_classification,
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
          false,
          exit_classification,
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
  worker_state->request_channels_closed.store(false, std::memory_order_release);
  worker_state->active_execution_count.store(0, std::memory_order_release);
  worker_state->idle_execution_count.store(worker_spawn_config_.max_threads, std::memory_order_release);
  worker_state->local_queue_depth.store(0, std::memory_order_release);
  worker_state->oldest_local_queue_age_nanoseconds.store(0, std::memory_order_release);
  worker_state->unexpected_exit_count.store(0, std::memory_order_release);
  worker_state->last_unexpected_exit_nanoseconds.store(0, std::memory_order_release);
  worker_state->last_exit_classification.store(WorkerExitClassification::none, std::memory_order_release);
  worker_state->last_exit_detail.store(0, std::memory_order_release);
  worker_state->recovery_requested.store(false, std::memory_order_release);
  worker_state->health_state.store(WorkerHealthState::healthy, std::memory_order_release);
  worker_state->last_health_transition_nanoseconds.store(steady_clock_nanoseconds(), std::memory_order_release);
  mark_lifecycle_transition(worker_state, WorkerLifecycleState::booting);
}

void Vajra::runtime::NativeRuntime::close_worker_request_channels(const std::shared_ptr<SharedWorkerState> &worker_state)
{
  bool expected = false;
  if (!worker_state->request_channels_closed.compare_exchange_strong(expected, true))
  {
    return;
  }

  std::vector<int> request_channel_fds;
  {
    const std::lock_guard<std::mutex> lock(worker_state->request_channel_mutex);
    request_channel_fds = worker_state->request_channel_fds;
  }

  for (int request_channel_fd : request_channel_fds)
  {
    shutdown(request_channel_fd, SHUT_RDWR);
    close_fd_if_open(request_channel_fd);
  }
}

bool Vajra::runtime::NativeRuntime::boot_replacement_worker(
    const std::shared_ptr<SharedWorkerState> &worker_state,
    const WorkerSpawnConfig &spawn_config,
    pid_t &pid,
    std::vector<int> &parent_request_channels,
    BootDiagnostic &failure_diagnostic)
{
  int readiness_pipe[2] = {-1, -1};
  if (pipe(readiness_pipe) != 0)
  {
    throw std::runtime_error(
        std::string("worker bootstrap pipe creation failed: ") + std::strerror(errno));
  }

  std::vector<std::array<int, 2>> request_channels;
  request_channels.reserve(spawn_config.max_threads);
  try
  {
    for (std::size_t thread_index = 0; thread_index < spawn_config.max_threads; ++thread_index)
    {
      std::array<int, 2> request_channel = {-1, -1};
      if (socketpair(AF_UNIX, SOCK_STREAM, 0, request_channel.data()) != 0)
      {
        throw std::runtime_error(
            std::string("worker request channel creation failed: ") + std::strerror(errno));
      }
      request_channels.push_back(request_channel);
    }
  }
  catch (...)
  {
    close_fd_if_open(readiness_pipe[0]);
    close_fd_if_open(readiness_pipe[1]);
    for (const auto &pair : request_channels)
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
        spawn_config.max_threads,
        spawn_config.port,
        spawn_config.max_request_head_bytes,
        readiness_pipe[1],
        static_cast<int>(worker_state->worker_index),
        spawn_config.worker_processes,
        spawn_config.debug_logging);
  }

  parent_request_channels.clear();
  parent_request_channels.reserve(request_channels.size());
  for (const auto &pair : request_channels)
  {
    close_fd_if_open(pair[1]);
    parent_request_channels.push_back(pair[0]);
  }
  close_fd_if_open(readiness_pipe[1]);

  {
    const std::lock_guard<std::mutex> lock(worker_state->request_channel_mutex);
    worker_state->request_channel_fds = parent_request_channels;
    worker_state->channel_generation.fetch_add(1, std::memory_order_acq_rel);
  }
  worker_state->pid.store(pid, std::memory_order_release);
  prepare_worker_replacement(worker_state);

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

void Vajra::runtime::NativeRuntime::replace_worker(const std::shared_ptr<SharedWorkerState> &worker_state)
{
  const std::uint64_t attempt_number =
      worker_state->replacement_attempt_count.fetch_add(1, std::memory_order_acq_rel) + 1;
  if (attempt_number > recovery_policy_.replacement_failure_limit)
  {
    worker_state->replacement_failure_count.fetch_add(1, std::memory_order_acq_rel);
    worker_state->replacement_needed.store(false, std::memory_order_release);
    if (debug_logging_.load(std::memory_order_acquire))
    {
      log_worker_lifecycle_event(
          "worker_replacement_failed",
          worker_state->worker_index,
          worker_state->pid.load(std::memory_order_acquire),
          worker_state->lifecycle_state.load(std::memory_order_acquire),
          worker_state->health_state.load(std::memory_order_acquire),
          false,
          worker_state->last_exit_classification.load(std::memory_order_acquire),
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
        false,
        worker_state->last_exit_classification.load(std::memory_order_acquire),
        true,
        static_cast<int>(attempt_number));
  }

  pid_t pid = -1;
  std::vector<int> parent_request_channels;
  BootDiagnostic failure_diagnostic{"worker_replacement_failed", "boot", "worker replacement bootstrap failed"};
  bool ready = false;
  try
  {
    ready = boot_replacement_worker(worker_state, worker_spawn_config_, pid, parent_request_channels, failure_diagnostic);
  }
  catch (...)
  {
    worker_state->replacement_failure_count.fetch_add(1, std::memory_order_acq_rel);
    worker_state->replacement_needed.store(true, std::memory_order_release);
    throw;
  }

  if (!ready)
  {
    worker_state->replacement_failure_count.fetch_add(1, std::memory_order_acq_rel);
    worker_state->replacement_needed.store(true, std::memory_order_release);
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

  if (debug_logging_.load(std::memory_order_acquire))
  {
    log_worker_lifecycle_event(
        "worker_replacement_ready",
        worker_state->worker_index,
        pid,
        WorkerLifecycleState::ready,
        WorkerHealthState::healthy,
        true,
        WorkerExitClassification::none,
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
  worker_state->timeout_kill_deadline_nanoseconds.store(
      steady_clock_nanoseconds_after(kWorkerTimeoutGracePeriod),
      std::memory_order_release);
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
    if (worker_state->lifecycle_state.load(std::memory_order_acquire) != WorkerLifecycleState::ready)
    {
      continue;
    }

    const WorkerHealthState health_state = worker_state->health_state.load(std::memory_order_acquire);
    if (health_state != WorkerHealthState::suspect &&
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

  for (const auto &worker_state : worker_states)
  {
    close_worker_request_channels(worker_state);
  }

  stop_worker_exit_watcher();
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
      sampled_last_progress = worker_state->last_progress_nanoseconds.load(std::memory_order_acquire);
      const std::int64_t progress_age = sampled_last_progress == 0 ? 0 : (now_nanoseconds - sampled_last_progress);
      const std::uint64_t unexpected_exits = worker_state->unexpected_exit_count.load(std::memory_order_acquire);
      const std::int64_t last_unexpected_exit =
          worker_state->last_unexpected_exit_nanoseconds.load(std::memory_order_acquire);
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
      else if (total_executions > 0 && active_executions >= total_executions)
      {
        next_state = WorkerHealthState::busy;
      }
      else
      {
        next_state = WorkerHealthState::healthy;
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

      if (debug_logging_.load(std::memory_order_acquire))
      {
        log_worker_lifecycle_event(
            "worker_health_changed",
            worker_state->worker_index,
            worker_state->pid.load(std::memory_order_acquire),
            lifecycle_state,
            next_state,
            worker_state->available.load(std::memory_order_acquire),
            worker_state->last_exit_classification.load(std::memory_order_acquire),
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
      log_worker_bootstrap_ready(port, boot_result.runtime_role, worker_processes);
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
    const std::vector<std::shared_ptr<SharedWorkerState>> &worker_states,
    bool debug_logging)
{
  auto server = std::make_shared<Vajra::Server>(
      config.port,
      config.host,
      config.max_request_head_bytes,
      std::make_shared<Vajra::rack::RackRequestExecutor>(
          Vajra::rack::request_channel_transport(
              worker_states,
              config.min_threads,
              config.queue_capacity,
              config.request_timeout_seconds,
              config.worker_timeout_seconds,
              [this](const std::shared_ptr<SharedWorkerState> &worker_state) {
                handle_worker_timeout(worker_state);
              },
              debug_logging),
          Vajra::rack::ControlPlaneConfig{config.stats_path, config.metrics_endpoint}),
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
  try
  {
    server_ptr->start();
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
    {
      const std::lock_guard<std::mutex> lock(server_mutex_);
      health_policy_ = health_policy_for(config);
      worker_spawn_config_ = WorkerSpawnConfig{
          config.max_threads,
          config.port,
          config.max_request_head_bytes,
          config.workers,
          debug_logging};
      recovery_policy_ = RecoveryPolicy{kReplacementFailureLimit};
      debug_logging_.store(debug_logging, std::memory_order_release);
    }
    configure_runtime_logging(config.structured_logs, config.access_log, config.error_log);
    log_runtime_banner_start(config.host, config.port, config.workers, config.min_threads, config.max_threads);
    flush_runtime_logs();
    const BootContractResult master_boot_result = BootContract::run(
        BootContractConfig{config.port, config.max_request_head_bytes, kMasterPreloadRuntimeRole});
    BootContract::ensure_ready(master_boot_result);

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
      const std::shared_ptr<SharedWorkerState> worker_state =
          register_worker_runtime(static_cast<std::size_t>(worker_index), pid, std::move(worker_parent_channels));
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
    std::atomic_bool server_thread_completed{false};
    std::mutex server_thread_mutex;
    std::string server_thread_error;
    const RuntimeConfig server_config = config;
    const std::vector<std::shared_ptr<SharedWorkerState>> server_worker_states = booted_worker_states;
    std::thread server_thread([this, server_config, server_worker_states, debug_logging, &server_thread_completed, &server_thread_mutex, &server_thread_error]() mutable {
      try
      {
        run_master_runtime_server(server_config, server_worker_states, debug_logging);
      }
      catch (const std::exception &error)
      {
        const std::lock_guard<std::mutex> lock(server_thread_mutex);
        server_thread_error = error.what();
      }
      catch (...)
      {
        const std::lock_guard<std::mutex> lock(server_thread_mutex);
        server_thread_error = "server failed with an unknown native error";
      }
      server_thread_completed.store(true, std::memory_order_release);
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
      if (server_thread_completed.load(std::memory_order_acquire))
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

    if (server_thread.joinable())
    {
      if (!server_thread_completed.load(std::memory_order_acquire))
      {
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
      server_thread.join();
    }
    {
      const std::lock_guard<std::mutex> lock(server_thread_mutex);
      if (!server_thread_error.empty())
      {
        throw std::runtime_error(server_thread_error);
      }
    }

    begin_runtime_shutdown();
    stop_worker_processes();
    wait_for_worker_exit(booted_worker_states);
    clear_worker_runtime();
    if (debug_logging)
    {
      log_runtime_stop_completed();
    }
    log_runtime_shutdown_complete();
  }
  catch (...)
  {
    begin_runtime_shutdown();
    stop_worker_processes();
    const std::vector<std::shared_ptr<SharedWorkerState>> live_worker_states = worker_states();
    if (!live_worker_states.empty())
    {
      wait_for_worker_exit(live_worker_states);
    }
    clear_worker_runtime();
    throw;
  }
}

void Vajra::runtime::NativeRuntime::stop()
{
  const bool had_runtime = runtime_running();
  if (had_runtime)
  {
    begin_runtime_shutdown();
  }

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
    std::string log_level,
    std::string access_log,
    std::string error_log,
    bool structured_logs,
    std::string stats_path,
    std::string metrics_endpoint)
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
      std::move(log_level),
      std::move(access_log),
      std::move(error_log),
      structured_logs,
      std::move(stats_path),
      std::move(metrics_endpoint)});
}

void VajraNative::stop()
{
  Vajra::runtime::NativeRuntime::instance().stop();
}
