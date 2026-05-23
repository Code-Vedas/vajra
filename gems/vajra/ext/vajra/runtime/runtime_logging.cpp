// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "runtime/runtime_logging.hpp"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <unistd.h>

std::string Vajra::runtime::utc_timestamp()
{
  const auto now = std::chrono::system_clock::now();
  const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
  std::tm utc_time{};
  gmtime_r(&now_time, &utc_time);

  std::ostringstream timestamp;
  timestamp << std::put_time(&utc_time, "%Y-%m-%dT%H:%M:%SZ");
  return timestamp.str();
}

std::string Vajra::runtime::runtime_environment_name()
{
  if (const char *rails_env = std::getenv("RAILS_ENV"); rails_env != nullptr && rails_env[0] != '\0')
  {
    return rails_env;
  }
  if (const char *rack_env = std::getenv("RACK_ENV"); rack_env != nullptr && rack_env[0] != '\0')
  {
    return rack_env;
  }

  return "development";
}

bool Vajra::runtime::debug_logging_enabled(const std::string &log_level)
{
  return log_level == "debug";
}

namespace
{
  const char *worker_lifecycle_state_name(Vajra::runtime::WorkerLifecycleState lifecycle_state)
  {
    switch (lifecycle_state)
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

  const char *worker_exit_classification_name(Vajra::runtime::WorkerExitClassification classification)
  {
    switch (classification)
    {
      case Vajra::runtime::WorkerExitClassification::none:
        return "none";
      case Vajra::runtime::WorkerExitClassification::expected_shutdown:
        return "expected_shutdown";
      case Vajra::runtime::WorkerExitClassification::exit_before_ready:
        return "exit_before_ready";
      case Vajra::runtime::WorkerExitClassification::unexpected_status:
        return "unexpected_status";
      case Vajra::runtime::WorkerExitClassification::unexpected_signal:
        return "unexpected_signal";
      case Vajra::runtime::WorkerExitClassification::unexpected_exit:
        return "unexpected_exit";
    }

    return "unknown";
  }
}

void Vajra::runtime::log_runtime_banner_start(
    const std::string &host,
    int port,
    int workers,
    std::size_t min_threads,
    std::size_t max_threads)
{
  const pid_t pid = getpid();
  std::cout << "[" << pid << "] === vajra boot: " << utc_timestamp() << " ===" << std::endl;
  std::cout << "[" << pid << "] Vajra starting in master-worker mode..." << std::endl;
  std::cout << "[" << pid << "] * Environment: " << runtime_environment_name() << std::endl;
  std::cout << "[" << pid << "] * Master PID: " << pid << std::endl;
  std::cout << "[" << pid << "] * Workers: " << workers << std::endl;
  std::cout << "[" << pid << "] * Min threads: " << min_threads << std::endl;
  std::cout << "[" << pid << "] * Max threads: " << max_threads << std::endl;
  std::cout << "[" << pid << "] * Bind: http://" << host << ":" << port << std::endl;
}

void Vajra::runtime::log_worker_lifecycle_event(
    const char *event_name,
    std::size_t worker_index,
    pid_t pid,
    WorkerLifecycleState lifecycle_state,
    bool available,
    WorkerExitClassification exit_classification,
    bool replacement_needed,
    int exit_detail)
{
  std::cout << "[Vajra][lifecycle] " << utc_timestamp()
            << " event=" << event_name
            << " worker_index=" << worker_index
            << " pid=" << pid
            << " lifecycle=" << worker_lifecycle_state_name(lifecycle_state)
            << " availability=" << (available ? "available" : "unavailable")
            << " exit_classification=" << worker_exit_classification_name(exit_classification)
            << " replacement_needed=" << (replacement_needed ? "true" : "false");
  if (exit_detail != 0)
  {
    std::cout << " exit_detail=" << exit_detail;
  }
  std::cout << std::endl;
}

void Vajra::runtime::log_unexpected_worker_exit(
    WorkerExitClassification exit_classification,
    int exit_detail)
{
  switch (exit_classification)
  {
    case WorkerExitClassification::unexpected_status:
      std::cout << "worker process exited unexpectedly with status " << exit_detail << std::endl;
      return;
    case WorkerExitClassification::unexpected_signal:
      std::cout << "worker process exited unexpectedly due to signal " << exit_detail << std::endl;
      return;
    case WorkerExitClassification::unexpected_exit:
      std::cout << "worker process exited unexpectedly" << std::endl;
      return;
    default:
      return;
  }
}

void Vajra::runtime::log_worker_bootstrap_ready(
    int port,
    const std::string &runtime_role,
    int worker_processes)
{
  std::cout << "[Vajra][lifecycle] " << utc_timestamp()
            << " event=worker_bootstrap_ready state=booting boot_status=ready stop_reason=none"
            << " port=" << port
            << " listener_owned=false listener_fd=-1"
            << " mode=master_worker"
            << " process_role=" << runtime_role
            << " request_execution_role=" << runtime_role
            << " worker_processes=" << worker_processes
            << std::endl;
}

void Vajra::runtime::log_worker_booted(int worker_index, pid_t pid, double boot_seconds)
{
  std::ostringstream message;
  message << "[" << getppid() << "] - Worker " << worker_index
          << " (PID: " << pid << ") booted in "
          << std::fixed << std::setprecision(2) << boot_seconds << "s";
  std::cout << message.str() << std::endl;
}

void Vajra::runtime::log_runtime_shutdown_begin()
{
  std::cout << "[" << getpid() << "] - Gracefully shutting down workers..." << std::endl;
}

void Vajra::runtime::log_runtime_shutdown_complete()
{
  std::cout << "[" << getpid() << "] === vajra shutdown: " << utc_timestamp() << " ===" << std::endl;
  std::cout << "[" << getpid() << "] - Goodbye!" << std::endl;
}
