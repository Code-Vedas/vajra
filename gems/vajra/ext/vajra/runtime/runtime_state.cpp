// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "runtime/runtime_state.hpp"
#include "runtime/runtime_logging.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <new>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <unordered_map>
#include <unistd.h>

namespace
{
  constexpr std::chrono::seconds kRssSampleRefreshInterval(1);

  struct RssSample
  {
    std::int64_t bytes = -1;
    std::chrono::steady_clock::time_point refreshed_at{};
  };

  Vajra::runtime::RuntimeState *installed_runtime_state = nullptr;
  thread_local Vajra::runtime::WorkerRuntimeState *installed_worker_state = nullptr;
  thread_local std::size_t installed_worker_index = 0;

  std::int64_t steady_clock_nanoseconds()
  {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }

  const char *worker_lifecycle_state_name(Vajra::runtime::WorkerLifecycleState lifecycle_state)
  {
    switch (lifecycle_state)
    {
      case Vajra::runtime::WorkerLifecycleState::booting: return "booting";
      case Vajra::runtime::WorkerLifecycleState::ready: return "ready";
      case Vajra::runtime::WorkerLifecycleState::stopping: return "stopping";
      case Vajra::runtime::WorkerLifecycleState::exited: return "exited";
    }
    return "unknown";
  }

  const char *worker_health_state_name(Vajra::runtime::WorkerHealthState state)
  {
    switch (state)
    {
      case Vajra::runtime::WorkerHealthState::healthy: return "healthy";
      case Vajra::runtime::WorkerHealthState::busy: return "busy";
      case Vajra::runtime::WorkerHealthState::overloaded: return "overloaded";
      case Vajra::runtime::WorkerHealthState::degraded: return "degraded";
      case Vajra::runtime::WorkerHealthState::suspect: return "suspect";
      case Vajra::runtime::WorkerHealthState::wedged: return "wedged";
    }
    return "unknown";
  }

#if defined(__linux__)
  std::int64_t rss_bytes_for_pid_from_proc(pid_t pid)
  {
    std::ifstream statm("/proc/" + std::to_string(pid) + "/statm");
    long long total_pages = 0;
    long long resident_pages = 0;
    statm >> total_pages >> resident_pages;
    if (statm.fail() || resident_pages < 0)
    {
      return -1;
    }

    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0)
    {
      return -1;
    }

    return static_cast<std::int64_t>(resident_pages) * static_cast<std::int64_t>(page_size);
  }
#endif

  std::int64_t rss_bytes_for_pid_from_ps(pid_t pid)
  {
    if (pid <= 0)
    {
      return -1;
    }

    const std::string command = "ps -o rss= -p " + std::to_string(pid);
    FILE *pipe = popen(command.c_str(), "r");
    if (pipe == nullptr)
    {
      return -1;
    }

    char buffer[128];
    std::string output;
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr)
    {
      output.append(buffer);
    }
    pclose(pipe);

    std::stringstream parser(output);
    long rss_kilobytes = 0;
    parser >> rss_kilobytes;
    if (parser.fail() || rss_kilobytes < 0)
    {
      return -1;
    }

    return static_cast<std::int64_t>(rss_kilobytes) * 1024;
  }

  std::int64_t cached_rss_bytes_for_pid_from_ps(pid_t pid)
  {
    static std::mutex cache_mutex;
    static std::unordered_map<pid_t, RssSample> cache;

    const auto now = std::chrono::steady_clock::now();
    {
      std::lock_guard<std::mutex> lock(cache_mutex);
      const auto cached = cache.find(pid);
      if (cached != cache.end() && now - cached->second.refreshed_at < kRssSampleRefreshInterval)
      {
        return cached->second.bytes;
      }
    }

    const std::int64_t bytes = rss_bytes_for_pid_from_ps(pid);
    {
      std::lock_guard<std::mutex> lock(cache_mutex);
      cache[pid] = RssSample{bytes, now};
    }

    return bytes;
  }

  std::int64_t rss_bytes_for_pid(pid_t pid)
  {
    if (pid <= 0)
    {
      return -1;
    }

#if defined(__linux__)
    const std::int64_t proc_rss_bytes = rss_bytes_for_pid_from_proc(pid);
    if (proc_rss_bytes >= 0)
    {
      return proc_rss_bytes;
    }
#endif

    return cached_rss_bytes_for_pid_from_ps(pid);
  }

  Vajra::runtime::WorkerRuntimeState *worker_state_at(std::size_t worker_index)
  {
    if (installed_runtime_state == nullptr || worker_index >= Vajra::runtime::kMaxTrackedWorkers)
    {
      return nullptr;
    }
    return &installed_runtime_state->workers[worker_index];
  }

  const char *health_state_name(Vajra::runtime::WorkerHealthState state)
  {
    switch (state)
    {
      case Vajra::runtime::WorkerHealthState::healthy:
        return "healthy";
      case Vajra::runtime::WorkerHealthState::busy:
        return "busy";
      case Vajra::runtime::WorkerHealthState::overloaded:
        return "overloaded";
      case Vajra::runtime::WorkerHealthState::degraded:
        return "degraded";
      case Vajra::runtime::WorkerHealthState::suspect:
        return "suspect";
      case Vajra::runtime::WorkerHealthState::wedged:
        return "wedged";
    }

    return "unknown";
  }

  const char *lifecycle_state_name(Vajra::runtime::WorkerLifecycleState state)
  {
    switch (state)
    {
      case Vajra::runtime::WorkerLifecycleState::booting:
        return "booting";
      case Vajra::runtime::WorkerLifecycleState::ready:
        return "ready";
      case Vajra::runtime::WorkerLifecycleState::stopping:
        return "stopping";
      case Vajra::runtime::WorkerLifecycleState::exited:
        return "exited";
    }

    return "unknown";
  }

  const char *recovery_state_name(Vajra::runtime::WorkerRecoveryState state)
  {
    switch (state)
    {
      case Vajra::runtime::WorkerRecoveryState::none:
        return "none";
      case Vajra::runtime::WorkerRecoveryState::draining:
        return "draining";
      case Vajra::runtime::WorkerRecoveryState::terminating:
        return "terminating";
      case Vajra::runtime::WorkerRecoveryState::replacing:
        return "replacing";
      case Vajra::runtime::WorkerRecoveryState::rejoin_pending:
        return "rejoin_pending";
      case Vajra::runtime::WorkerRecoveryState::terminal_failure:
        return "terminal_failure";
    }

    return "unknown";
  }

  std::string escaped_json_string(const std::string &value)
  {
    std::ostringstream escaped;
    escaped << '"';
    for (unsigned char character : value)
    {
      switch (character)
      {
        case '\\':
        case '"':
          escaped << '\\' << character;
          break;
        case '\b':
          escaped << "\\b";
          break;
        case '\f':
          escaped << "\\f";
          break;
        case '\n':
          escaped << "\\n";
          break;
        case '\r':
          escaped << "\\r";
          break;
        case '\t':
          escaped << "\\t";
          break;
        default:
          if (character < 0x20)
          {
            escaped << "\\u00";
            const char hex[] = "0123456789abcdef";
            escaped << hex[(character >> 4) & 0x0f] << hex[character & 0x0f];
          }
          else
          {
            escaped << static_cast<char>(character);
          }
      }
    }
    escaped << '"';
    return escaped.str();
  }

}

Vajra::runtime::RuntimeState *Vajra::runtime::allocate_runtime_state()
{
  void *region = mmap(nullptr, sizeof(RuntimeState), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
  if (region == MAP_FAILED)
  {
    throw std::runtime_error("failed to allocate shared runtime state");
  }

  return new (region) RuntimeState();
}

void Vajra::runtime::release_runtime_state(RuntimeState *state)
{
  if (state != nullptr)
  {
    if (installed_runtime_state == state)
    {
      installed_runtime_state = nullptr;
      installed_worker_state = nullptr;
      installed_worker_index = 0;
    }
    state->~RuntimeState();
    munmap(state, sizeof(RuntimeState));
  }
}

void Vajra::runtime::install_master_runtime_state(
    RuntimeState *state,
    std::size_t worker_count,
    std::size_t threads_per_worker,
    std::size_t socket_queue_capacity)
{
  installed_runtime_state = state;
  installed_worker_state = nullptr;
  installed_worker_index = 0;
  if (state == nullptr)
  {
    return;
  }

  state->master_pid.store(getpid(), std::memory_order_release);
  state->worker_count.store(static_cast<std::uint32_t>(worker_count), std::memory_order_release);
  state->threads_per_worker.store(static_cast<std::uint32_t>(threads_per_worker), std::memory_order_release);
  state->socket_queue_capacity.store(static_cast<std::uint32_t>(socket_queue_capacity), std::memory_order_release);
  state->shutdown_requested.store(false, std::memory_order_release);
}

void Vajra::runtime::install_worker_runtime_state(RuntimeState *state, std::size_t worker_index, pid_t pid)
{
  installed_runtime_state = state;
  attach_current_thread_to_worker_runtime_state(worker_index);
  if (installed_worker_state == nullptr)
  {
    return;
  }

  installed_worker_state->pid.store(pid, std::memory_order_release);
  installed_worker_state->lifecycle_state.store(
      static_cast<std::uint8_t>(WorkerLifecycleState::booting),
      std::memory_order_release);
  installed_worker_state->health_state.store(
      static_cast<std::uint8_t>(WorkerHealthState::healthy),
      std::memory_order_release);
  installed_worker_state->idle_execution_count.store(
      state->threads_per_worker.load(std::memory_order_acquire),
      std::memory_order_release);
  installed_worker_state->last_progress_nanoseconds.store(steady_clock_nanoseconds(), std::memory_order_release);
}

void Vajra::runtime::attach_current_thread_to_worker_runtime_state(std::size_t worker_index)
{
  installed_worker_index = worker_index;
  installed_worker_state = worker_state_at(worker_index);
}

void Vajra::runtime::detach_worker_runtime_state()
{
  installed_worker_state = nullptr;
}

Vajra::runtime::RuntimeState *Vajra::runtime::current_runtime_state()
{
  return installed_runtime_state;
}

Vajra::runtime::WorkerRuntimeState *Vajra::runtime::current_worker_runtime_state()
{
  return installed_worker_state;
}

std::size_t Vajra::runtime::current_worker_index()
{
  return installed_worker_index;
}

void Vajra::runtime::set_runtime_listener_fd(int listener_fd)
{
  if (installed_runtime_state != nullptr)
  {
    installed_runtime_state->listener_fd.store(listener_fd, std::memory_order_release);
  }
}

int Vajra::runtime::runtime_listener_fd()
{
  if (installed_runtime_state == nullptr)
  {
    return -1;
  }
  return installed_runtime_state->listener_fd.load(std::memory_order_acquire);
}

void Vajra::runtime::mark_runtime_shutdown_requested()
{
  if (installed_runtime_state != nullptr)
  {
    installed_runtime_state->shutdown_requested.store(true, std::memory_order_release);
  }
}

void Vajra::runtime::mark_worker_lifecycle(std::size_t worker_index, WorkerLifecycleState lifecycle_state)
{
  WorkerRuntimeState *state = worker_state_at(worker_index);
  if (state != nullptr)
  {
    state->lifecycle_state.store(static_cast<std::uint8_t>(lifecycle_state), std::memory_order_release);
    state->last_progress_nanoseconds.store(steady_clock_nanoseconds(), std::memory_order_release);
  }
}

void Vajra::runtime::mark_worker_recovery(std::size_t worker_index, WorkerRecoveryState recovery_state)
{
  WorkerRuntimeState *state = worker_state_at(worker_index);
  if (state != nullptr)
  {
    state->recovery_state.store(static_cast<std::uint8_t>(recovery_state), std::memory_order_release);
  }
}

void Vajra::runtime::mark_worker_health(std::size_t worker_index, WorkerHealthState health_state)
{
  WorkerRuntimeState *state = worker_state_at(worker_index);
  if (state != nullptr)
  {
    state->health_state.store(static_cast<std::uint8_t>(health_state), std::memory_order_release);
  }
}

void Vajra::runtime::mark_worker_available(std::size_t worker_index, bool available)
{
  WorkerRuntimeState *state = worker_state_at(worker_index);
  if (state != nullptr)
  {
    state->available.store(available, std::memory_order_release);
  }
}

void Vajra::runtime::mark_worker_replacement_counters(
    std::size_t worker_index,
    std::uint64_t attempt_count,
    std::uint64_t success_count,
    std::uint64_t failure_count)
{
  WorkerRuntimeState *state = worker_state_at(worker_index);
  if (state != nullptr)
  {
    state->replacement_attempt_count.store(attempt_count, std::memory_order_release);
    state->replacement_success_count.store(success_count, std::memory_order_release);
    state->replacement_failure_count.store(failure_count, std::memory_order_release);
  }
}

void Vajra::runtime::mark_worker_timeout_escalations(std::size_t worker_index, std::uint64_t count)
{
  WorkerRuntimeState *state = worker_state_at(worker_index);
  if (state != nullptr)
  {
    state->timeout_escalation_count.store(count, std::memory_order_release);
  }
}

void Vajra::runtime::mark_worker_unexpected_exit(std::size_t worker_index, std::uint64_t count, std::int64_t last_exit_nanoseconds)
{
  WorkerRuntimeState *state = worker_state_at(worker_index);
  if (state != nullptr)
  {
    state->unexpected_exit_count.store(count, std::memory_order_release);
    state->last_unexpected_exit_nanoseconds.store(last_exit_nanoseconds, std::memory_order_release);
  }
}

void Vajra::runtime::mark_worker_terminal_replacement_failure(std::size_t worker_index, bool terminal_failure)
{
  WorkerRuntimeState *state = worker_state_at(worker_index);
  if (state != nullptr)
  {
    state->terminal_replacement_failure.store(terminal_failure, std::memory_order_release);
  }
}

void Vajra::runtime::note_worker_connection_opened()
{
  if (installed_worker_state == nullptr)
  {
    return;
  }
  installed_worker_state->active_connections.fetch_add(1, std::memory_order_acq_rel);
  installed_worker_state->last_progress_nanoseconds.store(steady_clock_nanoseconds(), std::memory_order_release);
}

void Vajra::runtime::note_worker_connection_closed()
{
  if (installed_worker_state == nullptr)
  {
    return;
  }
  const std::size_t previous = installed_worker_state->active_connections.fetch_sub(1, std::memory_order_acq_rel);
  if (previous == 0)
  {
    installed_worker_state->active_connections.store(0, std::memory_order_release);
  }
  installed_worker_state->last_progress_nanoseconds.store(steady_clock_nanoseconds(), std::memory_order_release);
}

void Vajra::runtime::note_worker_accept()
{
  if (installed_worker_state != nullptr)
  {
    installed_worker_state->accept_count.fetch_add(1, std::memory_order_acq_rel);
  }
}

void Vajra::runtime::note_worker_dispatch_received()
{
  if (installed_worker_state != nullptr)
  {
    installed_worker_state->receive_count.fetch_add(1, std::memory_order_acq_rel);
  }
}

void Vajra::runtime::note_worker_request_head_time(std::int64_t nanoseconds)
{
  if (installed_worker_state != nullptr)
  {
    installed_worker_state->request_head_nanoseconds.fetch_add(nanoseconds, std::memory_order_acq_rel);
  }
}

void Vajra::runtime::note_worker_request_parse_time(std::int64_t nanoseconds)
{
  if (installed_worker_state != nullptr)
  {
    installed_worker_state->request_parse_nanoseconds.fetch_add(nanoseconds, std::memory_order_acq_rel);
  }
}

void Vajra::runtime::note_worker_request_body_time(std::int64_t nanoseconds)
{
  if (installed_worker_state != nullptr)
  {
    installed_worker_state->request_body_nanoseconds.fetch_add(nanoseconds, std::memory_order_acq_rel);
  }
}

void Vajra::runtime::note_worker_request_time(std::int64_t nanoseconds)
{
  if (installed_worker_state != nullptr)
  {
    installed_worker_state->request_total_nanoseconds.fetch_add(nanoseconds, std::memory_order_acq_rel);
  }
}

void Vajra::runtime::note_worker_rack_start_time(std::int64_t nanoseconds)
{
  if (installed_worker_state != nullptr)
  {
    installed_worker_state->rack_start_nanoseconds.fetch_add(nanoseconds, std::memory_order_acq_rel);
  }
}

void Vajra::runtime::note_worker_rack_finish_time(std::int64_t nanoseconds)
{
  if (installed_worker_state != nullptr)
  {
    installed_worker_state->rack_finish_nanoseconds.fetch_add(nanoseconds, std::memory_order_acq_rel);
  }
}

void Vajra::runtime::note_worker_rack_execution_time(std::int64_t nanoseconds)
{
  if (installed_worker_state != nullptr)
  {
    installed_worker_state->rack_execution_nanoseconds.fetch_add(nanoseconds, std::memory_order_acq_rel);
  }
}

void Vajra::runtime::note_worker_response_write_time(std::int64_t nanoseconds)
{
  if (installed_worker_state != nullptr)
  {
    installed_worker_state->response_write_nanoseconds.fetch_add(nanoseconds, std::memory_order_acq_rel);
  }
}

void Vajra::runtime::note_worker_request_completed()
{
  if (installed_worker_state != nullptr)
  {
    installed_worker_state->completed_request_count.fetch_add(1, std::memory_order_acq_rel);
    installed_worker_state->last_progress_nanoseconds.store(steady_clock_nanoseconds(), std::memory_order_release);
  }
}

void Vajra::runtime::note_worker_execution_started()
{
  if (installed_worker_state == nullptr || installed_runtime_state == nullptr)
  {
    return;
  }
  const std::size_t active = installed_worker_state->active_execution_count.fetch_add(1, std::memory_order_acq_rel) + 1;
  const std::size_t threads_per_worker = installed_runtime_state->threads_per_worker.load(std::memory_order_acquire);
  installed_worker_state->idle_execution_count.store(
      active >= threads_per_worker ? 0 : threads_per_worker - active,
      std::memory_order_release);
}

void Vajra::runtime::note_worker_execution_finished()
{
  if (installed_worker_state == nullptr || installed_runtime_state == nullptr)
  {
    return;
  }
  const std::size_t previous = installed_worker_state->active_execution_count.fetch_sub(1, std::memory_order_acq_rel);
  const std::size_t active = previous == 0 ? 0 : previous - 1;
  const std::size_t threads_per_worker = installed_runtime_state->threads_per_worker.load(std::memory_order_acquire);
  installed_worker_state->active_execution_count.store(active, std::memory_order_release);
  installed_worker_state->idle_execution_count.store(
      active >= threads_per_worker ? 0 : threads_per_worker - active,
      std::memory_order_release);
}

void Vajra::runtime::note_worker_local_queue_depth(std::size_t queue_depth)
{
  if (installed_worker_state != nullptr)
  {
    installed_worker_state->local_queue_depth.store(queue_depth, std::memory_order_release);
  }
}

void Vajra::runtime::note_master_dispatch(std::size_t worker_index, std::int64_t nanoseconds)
{
  WorkerRuntimeState *state = worker_state_at(worker_index);
  if (state != nullptr)
  {
    state->dispatch_count.fetch_add(1, std::memory_order_acq_rel);
    state->last_progress_nanoseconds.store(steady_clock_nanoseconds(), std::memory_order_release);
    (void)nanoseconds;
  }
}

void Vajra::runtime::note_master_fd_transfer_failure(std::size_t worker_index)
{
  WorkerRuntimeState *state = worker_state_at(worker_index);
  if (state != nullptr)
  {
    state->fd_transfer_failures.fetch_add(1, std::memory_order_acq_rel);
  }
}

std::string Vajra::runtime::runtime_stats_payload_json()
{
  if (installed_runtime_state == nullptr)
  {
    return "{\"workers\":[]}";
  }

  std::uint64_t healthy = 0;
  std::uint64_t busy = 0;
  std::uint64_t overloaded = 0;
  std::uint64_t degraded = 0;
  std::uint64_t suspect = 0;
  std::uint64_t wedged = 0;
  std::int64_t total_head_ns = 0;
  std::int64_t total_parse_ns = 0;
  std::int64_t total_body_ns = 0;
  std::int64_t total_request_ns = 0;
  std::int64_t total_rack_start_ns = 0;
  std::int64_t total_rack_finish_ns = 0;
  std::int64_t total_rack_ns = 0;
  std::int64_t total_response_ns = 0;
  std::uint64_t total_accepts = 0;
  std::uint64_t total_dispatches = 0;
  std::uint64_t total_receives = 0;
  std::uint64_t total_fd_transfer_failures = 0;

  std::ostringstream payload;
  payload << '{'
          << "\"master_pid\":" << installed_runtime_state->master_pid.load(std::memory_order_acquire) << ','
          << "\"master_rss_bytes\":" << rss_bytes_for_pid(installed_runtime_state->master_pid.load(std::memory_order_acquire)) << ','
          << "\"socket_queue_capacity\":" << installed_runtime_state->socket_queue_capacity.load(std::memory_order_acquire) << ','
          << "\"workers\":[";

  const std::size_t worker_count = installed_runtime_state->worker_count.load(std::memory_order_acquire);
  for (std::size_t index = 0; index < worker_count; ++index)
  {
    if (index > 0)
    {
      payload << ',';
    }
    const WorkerRuntimeState &worker = installed_runtime_state->workers[index];
    const WorkerHealthState health_state =
        static_cast<WorkerHealthState>(worker.health_state.load(std::memory_order_acquire));
    switch (health_state)
    {
      case WorkerHealthState::healthy: ++healthy; break;
      case WorkerHealthState::busy: ++busy; break;
      case WorkerHealthState::overloaded: ++overloaded; break;
      case WorkerHealthState::degraded: ++degraded; break;
      case WorkerHealthState::suspect: ++suspect; break;
      case WorkerHealthState::wedged: ++wedged; break;
    }

    total_head_ns += worker.request_head_nanoseconds.load(std::memory_order_acquire);
    total_parse_ns += worker.request_parse_nanoseconds.load(std::memory_order_acquire);
    total_body_ns += worker.request_body_nanoseconds.load(std::memory_order_acquire);
    total_request_ns += worker.request_total_nanoseconds.load(std::memory_order_acquire);
    total_rack_start_ns += worker.rack_start_nanoseconds.load(std::memory_order_acquire);
    total_rack_finish_ns += worker.rack_finish_nanoseconds.load(std::memory_order_acquire);
    total_rack_ns += worker.rack_execution_nanoseconds.load(std::memory_order_acquire);
    total_response_ns += worker.response_write_nanoseconds.load(std::memory_order_acquire);
    total_accepts += worker.accept_count.load(std::memory_order_acquire);
    total_dispatches += worker.dispatch_count.load(std::memory_order_acquire);
    total_receives += worker.receive_count.load(std::memory_order_acquire);
    total_fd_transfer_failures += worker.fd_transfer_failures.load(std::memory_order_acquire);

    payload << '{'
            << "\"worker_index\":" << index << ','
            << "\"pid\":" << worker.pid.load(std::memory_order_acquire) << ','
            << "\"rss_bytes\":" << rss_bytes_for_pid(worker.pid.load(std::memory_order_acquire)) << ','
            << "\"active_connections\":" << worker.active_connections.load(std::memory_order_acquire) << ','
            << "\"active_execution_count\":" << worker.active_execution_count.load(std::memory_order_acquire) << ','
            << "\"idle_execution_count\":" << worker.idle_execution_count.load(std::memory_order_acquire) << ','
            << "\"local_queue_depth\":" << worker.local_queue_depth.load(std::memory_order_acquire) << ','
            << "\"available\":" << (worker.available.load(std::memory_order_acquire) ? "true" : "false") << ','
            << "\"lifecycle_state_name\":"
            << escaped_json_string(lifecycle_state_name(static_cast<WorkerLifecycleState>(
                   worker.lifecycle_state.load(std::memory_order_acquire))))
            << ','
            << "\"health_state_name\":"
            << escaped_json_string(health_state_name(health_state))
            << ','
            << "\"recovery_state_name\":"
            << escaped_json_string(recovery_state_name(static_cast<WorkerRecoveryState>(
                   worker.recovery_state.load(std::memory_order_acquire))))
            << ','
            << "\"accept_count\":" << worker.accept_count.load(std::memory_order_acquire) << ','
            << "\"dispatch_count\":" << worker.dispatch_count.load(std::memory_order_acquire) << ','
            << "\"receive_count\":" << worker.receive_count.load(std::memory_order_acquire) << ','
            << "\"fd_transfer_failures\":" << worker.fd_transfer_failures.load(std::memory_order_acquire) << ','
            << "\"completed_request_count\":" << worker.completed_request_count.load(std::memory_order_acquire) << ','
            << "\"last_progress_nanoseconds\":" << worker.last_progress_nanoseconds.load(std::memory_order_acquire) << ','
            << "\"replacement_attempt_count\":" << worker.replacement_attempt_count.load(std::memory_order_acquire) << ','
            << "\"replacement_success_count\":" << worker.replacement_success_count.load(std::memory_order_acquire) << ','
            << "\"replacement_failure_count\":" << worker.replacement_failure_count.load(std::memory_order_acquire) << ','
            << "\"timeout_escalation_count\":" << worker.timeout_escalation_count.load(std::memory_order_acquire) << ','
            << "\"unexpected_exit_count\":" << worker.unexpected_exit_count.load(std::memory_order_acquire) << ','
            << "\"last_unexpected_exit_nanoseconds\":" << worker.last_unexpected_exit_nanoseconds.load(std::memory_order_acquire) << ','
            << "\"overload_started_nanoseconds\":" << worker.overload_started_nanoseconds.load(std::memory_order_acquire) << ','
            << "\"recovery_deadline_nanoseconds\":" << worker.recovery_deadline_nanoseconds.load(std::memory_order_acquire) << ','
            << "\"terminal_replacement_failure\":" << (worker.terminal_replacement_failure.load(std::memory_order_acquire) ? "true" : "false")
            << '}';
  }

  payload << "],"
          << "\"profiling\":{"
          << "\"request_head_nanoseconds\":" << total_head_ns << ','
          << "\"request_parse_nanoseconds\":" << total_parse_ns << ','
          << "\"request_body_nanoseconds\":" << total_body_ns << ','
          << "\"request_total_nanoseconds\":" << total_request_ns << ','
          << "\"rack_start_nanoseconds\":" << total_rack_start_ns << ','
          << "\"rack_finish_nanoseconds\":" << total_rack_finish_ns << ','
          << "\"ruby_execution_nanoseconds\":" << total_rack_ns << ','
          << "\"response_write_nanoseconds\":" << total_response_ns << ','
          << "\"accept_count\":" << total_accepts << ','
          << "\"dispatch_count\":" << total_dispatches << ','
          << "\"receive_count\":" << total_receives << ','
          << "\"fd_transfer_failures\":" << total_fd_transfer_failures
          << "},"
          << "\"native_observability\":{"
          << "\"request_events_total\":" << Vajra::runtime::runtime_native_request_observability_events_total() << ','
          << "\"request_errors_total\":" << Vajra::runtime::runtime_native_request_observability_errors_total() << ','
          << "\"admission_rejections_total\":" << Vajra::runtime::runtime_native_request_admission_rejections_total()
          << "},"
          << "\"health_counts\":{"
          << "\"healthy\":" << healthy << ','
          << "\"busy\":" << busy << ','
          << "\"overloaded\":" << overloaded << ','
          << "\"degraded\":" << degraded << ','
          << "\"suspect\":" << suspect << ','
          << "\"wedged\":" << wedged
          << "}"
          << '}';
  return payload.str();
}

std::string Vajra::runtime::runtime_metrics_payload_text()
{
  if (installed_runtime_state == nullptr)
  {
    return "vajra_runtime_up 1\n";
  }

  std::ostringstream payload;
  payload << "vajra_runtime_up 1\n";
  const std::size_t worker_count = installed_runtime_state->worker_count.load(std::memory_order_acquire);
  for (std::size_t index = 0; index < worker_count; ++index)
  {
    const WorkerRuntimeState &worker = installed_runtime_state->workers[index];
    payload << "vajra_worker_active_connections{worker=\"" << index << "\"} "
            << worker.active_connections.load(std::memory_order_acquire) << '\n';
    payload << "vajra_worker_active_executions{worker=\"" << index << "\"} "
            << worker.active_execution_count.load(std::memory_order_acquire) << '\n';
    payload << "vajra_worker_idle_executions{worker=\"" << index << "\"} "
            << worker.idle_execution_count.load(std::memory_order_acquire) << '\n';
    payload << "vajra_worker_accept_total{worker=\"" << index << "\"} "
            << worker.accept_count.load(std::memory_order_acquire) << '\n';
    payload << "vajra_worker_dispatch_total{worker=\"" << index << "\"} "
            << worker.dispatch_count.load(std::memory_order_acquire) << '\n';
    payload << "vajra_worker_receive_total{worker=\"" << index << "\"} "
            << worker.receive_count.load(std::memory_order_acquire) << '\n';
    payload << "vajra_worker_completed_requests_total{worker=\"" << index << "\"} "
            << worker.completed_request_count.load(std::memory_order_acquire) << '\n';
    payload << "vajra_worker_request_head_nanoseconds_total{worker=\"" << index << "\"} "
            << worker.request_head_nanoseconds.load(std::memory_order_acquire) << '\n';
    payload << "vajra_worker_request_parse_nanoseconds_total{worker=\"" << index << "\"} "
            << worker.request_parse_nanoseconds.load(std::memory_order_acquire) << '\n';
    payload << "vajra_worker_request_body_nanoseconds_total{worker=\"" << index << "\"} "
            << worker.request_body_nanoseconds.load(std::memory_order_acquire) << '\n';
    payload << "vajra_worker_request_nanoseconds_total{worker=\"" << index << "\"} "
            << worker.request_total_nanoseconds.load(std::memory_order_acquire) << '\n';
    payload << "vajra_worker_rack_execution_nanoseconds_total{worker=\"" << index << "\"} "
            << worker.rack_execution_nanoseconds.load(std::memory_order_acquire) << '\n';
    payload << "vajra_worker_response_write_nanoseconds_total{worker=\"" << index << "\"} "
            << worker.response_write_nanoseconds.load(std::memory_order_acquire) << '\n';
    payload << "vajra_worker_local_queue_depth{worker=\"" << index << "\"} "
            << worker.local_queue_depth.load(std::memory_order_acquire) << '\n';
    payload << "vajra_worker_lifecycle_state{worker=\"" << index << "\",state=\""
            << worker_lifecycle_state_name(
                   static_cast<WorkerLifecycleState>(worker.lifecycle_state.load(std::memory_order_acquire)))
            << "\"} 1\n";
    payload << "vajra_worker_health_state{worker=\"" << index << "\",state=\""
            << worker_health_state_name(static_cast<WorkerHealthState>(worker.health_state.load(std::memory_order_acquire)))
            << "\"} 1\n";
    payload << "vajra_worker_replacement_attempts_total{worker=\"" << index << "\"} "
            << worker.replacement_attempt_count.load(std::memory_order_acquire) << '\n';
    payload << "vajra_worker_replacement_success_total{worker=\"" << index << "\"} "
            << worker.replacement_success_count.load(std::memory_order_acquire) << '\n';
    payload << "vajra_worker_replacement_failure_total{worker=\"" << index << "\"} "
            << worker.replacement_failure_count.load(std::memory_order_acquire) << '\n';
    payload << "vajra_worker_timeout_escalations_total{worker=\"" << index << "\"} "
            << worker.timeout_escalation_count.load(std::memory_order_acquire) << '\n';
    payload << "vajra_worker_unexpected_exits_total{worker=\"" << index << "\"} "
            << worker.unexpected_exit_count.load(std::memory_order_acquire) << '\n';
  }
  return payload.str();
}
