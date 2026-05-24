// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_RUNTIME_RUNTIME_LOGGING_HPP
#define VAJRA_RUNTIME_RUNTIME_LOGGING_HPP

#include "runtime/worker_pool.hpp"

#include <cstddef>
#include <string>
#include <sys/types.h>

namespace Vajra
{
  namespace runtime
  {
    std::string utc_timestamp();
    std::string runtime_environment_name();
    bool debug_logging_enabled(const std::string &log_level);
    void configure_runtime_logging(
        bool structured_logs,
        const std::string &access_log,
        const std::string &error_log);
    void log_runtime_banner_start(
        const std::string &host,
        int port,
        int workers,
        std::size_t min_threads,
        std::size_t max_threads);
    void log_worker_lifecycle_event(
        const char *event_name,
        std::size_t worker_index,
        pid_t pid,
        WorkerLifecycleState lifecycle_state,
        bool available,
        WorkerExitClassification exit_classification,
        bool replacement_needed,
        int exit_detail);
    void log_unexpected_worker_exit(WorkerExitClassification exit_classification, int exit_detail);
    void log_worker_bootstrap_ready(
        int port,
        const std::string &runtime_role,
        int worker_processes);
    void log_worker_booted(int worker_index, pid_t pid, double boot_seconds);
    void log_runtime_error(const std::string &message);
    void log_access_event(const std::string &method, const std::string &target, int status_code);
    void log_runtime_shutdown_begin();
    void log_runtime_shutdown_complete();
  }
}

#endif
