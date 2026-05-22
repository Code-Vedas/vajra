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

void Vajra::runtime::log_worker_booted(int worker_index, int pid, double boot_seconds)
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
