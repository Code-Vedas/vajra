// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_RUNTIME_RUNTIME_LOGGING_HPP
#define VAJRA_RUNTIME_RUNTIME_LOGGING_HPP

#include "runtime/worker_pool.hpp"

#include <cstddef>
#include <cstdint>
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
        const std::string &error_log,
        const std::string &access_log_format);
    void configure_runtime_tracing(
        bool trace_enabled,
        const std::string &trace_endpoint,
        const std::string &trace_service_name);
    void set_runtime_tracing_available(bool available);
    bool runtime_tracing_enabled();
    bool runtime_tracing_available();
    std::string runtime_tracing_endpoint();
    std::string runtime_tracing_service_name();
    void start_runtime_logging_worker();
    void stop_runtime_logging_worker();
    void set_runtime_lifecycle_callback(void *callback);
    void set_runtime_request_observability_callback(void *callback);
    void flush_runtime_logs();
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
        WorkerHealthState health_state,
        WorkerRecoveryState recovery_state,
        bool available,
        WorkerExitClassification exit_classification,
        bool terminal_replacement_failure,
        bool replacement_needed,
        int exit_detail);
    void log_unexpected_worker_exit(WorkerExitClassification exit_classification, int exit_detail);
    void log_worker_bootstrap_ready(
        int port,
        const std::string &runtime_role,
        int worker_processes);
    void log_worker_booted(int worker_index, pid_t pid, double boot_seconds);
    void log_runtime_error(const std::string &message);
    struct AccessLogEvent
    {
      std::string method;
      std::string target;
      int status_code = 0;
      std::int64_t duration_nanoseconds = 0;
      std::size_t response_body_bytes = 0;
      std::string remote_address;
      std::string protocol;
      std::string host;
      std::string user_agent;
      std::string referer;
      std::string request_id;
      pid_t worker_pid = -1;
      int worker_index = -1;
      std::string connection_outcome;
      std::string trace_id;
      std::string span_id;
    };
    void log_access_event(const AccessLogEvent &event);
    void emit_runtime_request_observability_event(
        const AccessLogEvent &event,
        const std::string &outcome,
        const std::string &failure_kind,
        bool response_sent,
        const std::string &error_message);
    void log_runtime_shutdown_begin();
    void log_runtime_stop_completed();
    void log_runtime_shutdown_complete();
  }
}

#endif
