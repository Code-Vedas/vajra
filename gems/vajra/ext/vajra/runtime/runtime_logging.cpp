// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "runtime/runtime_logging.hpp"

#if __has_include("ruby.h")
#include "ruby.h"
#include "ruby/thread.h"
#define VAJRA_RUNTIME_HAS_RUBY 1
#endif

#include <chrono>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <cstdint>
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
  struct LoggingConfig
  {
    bool structured_logs = false;
    bool trace_enabled = false;
    bool trace_available = false;
    std::string access_log_path;
    std::string error_log_path;
    std::string trace_endpoint;
    std::string trace_service_name;
    std::unique_ptr<std::ofstream> access_log_stream;
    std::unique_ptr<std::ofstream> error_log_stream;
  };

  std::mutex logging_mutex;
  LoggingConfig logging_config;
#ifdef VAJRA_RUNTIME_HAS_RUBY
  VALUE runtime_lifecycle_callback = Qnil;

  struct LifecycleCallbackContext
  {
    VALUE callback = Qnil;
    std::string event_name;
    std::size_t worker_index = 0;
    pid_t pid = -1;
    std::string lifecycle_state;
    std::string health_state;
    std::string recovery_state;
    bool available = false;
    std::string exit_classification;
    bool terminal_replacement_failure = false;
    bool replacement_needed = false;
    int exit_detail = 0;
    std::string timestamp;
  };
#endif

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

  const char *worker_health_state_name(Vajra::runtime::WorkerHealthState state)
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

  const char *worker_recovery_state_name(Vajra::runtime::WorkerRecoveryState state)
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

  std::string escaped_log_value(const std::string &value)
  {
    std::ostringstream escaped;
    escaped << '"';
    for (unsigned char character : value)
    {
      switch (character)
      {
        case '\\':
          escaped << "\\\\";
          break;
        case '"':
          escaped << "\\\"";
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
          if (std::isprint(character))
          {
            escaped << static_cast<char>(character);
          }
          else
          {
            escaped << "\\u00" << std::hex << std::setw(2) << std::setfill('0')
                    << static_cast<int>(character)
                    << std::dec << std::setfill(' ');
          }
          break;
      }
    }
    escaped << '"';
    return escaped.str();
  }

  void write_line(std::ostream &stream, const std::string &line)
  {
    stream << line << '\n';
  }

  void flush_runtime_streams()
  {
    std::lock_guard<std::mutex> lock(logging_mutex);
    std::cout.flush();
    std::cerr.flush();
    if (logging_config.access_log_stream)
    {
      logging_config.access_log_stream->flush();
    }
    if (logging_config.error_log_stream)
    {
      logging_config.error_log_stream->flush();
    }
  }

  bool structured_logging_enabled()
  {
    const std::lock_guard<std::mutex> lock(logging_mutex);
    return logging_config.structured_logs;
  }

#ifdef VAJRA_RUNTIME_HAS_RUBY
  VALUE invoke_runtime_lifecycle_callback(VALUE payload)
  {
    auto *callback_context = reinterpret_cast<LifecycleCallbackContext *>(payload);
    VALUE event = rb_hash_new();
    rb_hash_aset(event, ID2SYM(rb_intern("event")), rb_str_new_cstr(callback_context->event_name.c_str()));
    rb_hash_aset(event, ID2SYM(rb_intern("worker_index")), ULL2NUM(callback_context->worker_index));
    rb_hash_aset(event, ID2SYM(rb_intern("pid")), INT2NUM(callback_context->pid));
    rb_hash_aset(event, ID2SYM(rb_intern("lifecycle_state")), rb_str_new_cstr(callback_context->lifecycle_state.c_str()));
    rb_hash_aset(event, ID2SYM(rb_intern("health_state")), rb_str_new_cstr(callback_context->health_state.c_str()));
    rb_hash_aset(event, ID2SYM(rb_intern("recovery_state")), rb_str_new_cstr(callback_context->recovery_state.c_str()));
    rb_hash_aset(event, ID2SYM(rb_intern("available")), callback_context->available ? Qtrue : Qfalse);
    rb_hash_aset(event, ID2SYM(rb_intern("exit_classification")), rb_str_new_cstr(callback_context->exit_classification.c_str()));
    rb_hash_aset(
        event,
        ID2SYM(rb_intern("terminal_replacement_failure")),
        callback_context->terminal_replacement_failure ? Qtrue : Qfalse);
    rb_hash_aset(event, ID2SYM(rb_intern("replacement_needed")), callback_context->replacement_needed ? Qtrue : Qfalse);
    rb_hash_aset(event, ID2SYM(rb_intern("exit_detail")), INT2NUM(callback_context->exit_detail));
    rb_hash_aset(event, ID2SYM(rb_intern("timestamp")), rb_str_new_cstr(callback_context->timestamp.c_str()));
    return rb_funcall(callback_context->callback, rb_intern("call"), 1, event);
  }

  void *emit_lifecycle_callback_with_gvl(void *data)
  {
    auto *context = reinterpret_cast<LifecycleCallbackContext *>(data);
    if (NIL_P(context->callback))
    {
      return nullptr;
    }

    int state = 0;
    rb_protect(invoke_runtime_lifecycle_callback, reinterpret_cast<VALUE>(context), &state);
    return nullptr;
  }
#endif

  void emit_runtime_lifecycle_callback(
      const char *event_name,
      std::size_t worker_index,
      pid_t pid,
      Vajra::runtime::WorkerLifecycleState lifecycle_state,
      Vajra::runtime::WorkerHealthState health_state,
      Vajra::runtime::WorkerRecoveryState recovery_state,
      bool available,
      Vajra::runtime::WorkerExitClassification exit_classification,
      bool terminal_replacement_failure,
      bool replacement_needed,
      int exit_detail)
  {
#ifdef VAJRA_RUNTIME_HAS_RUBY
    VALUE callback = Qnil;
    {
      const std::lock_guard<std::mutex> lock(logging_mutex);
      callback = runtime_lifecycle_callback;
    }
    if (NIL_P(callback))
    {
      return;
    }

    LifecycleCallbackContext context;
    context.callback = callback;
    context.event_name = event_name;
    context.worker_index = worker_index;
    context.pid = pid;
    context.lifecycle_state = worker_lifecycle_state_name(lifecycle_state);
    context.health_state = worker_health_state_name(health_state);
    context.recovery_state = worker_recovery_state_name(recovery_state);
    context.available = available;
    context.exit_classification = worker_exit_classification_name(exit_classification);
    context.terminal_replacement_failure = terminal_replacement_failure;
    context.replacement_needed = replacement_needed;
    context.exit_detail = exit_detail;
    context.timestamp = Vajra::runtime::utc_timestamp();
    rb_thread_call_with_gvl(emit_lifecycle_callback_with_gvl, &context);
#else
    (void)event_name;
    (void)worker_index;
    (void)pid;
    (void)lifecycle_state;
    (void)health_state;
    (void)recovery_state;
    (void)available;
    (void)exit_classification;
    (void)terminal_replacement_failure;
    (void)replacement_needed;
    (void)exit_detail;
#endif
  }

  void write_runtime_line(const std::string &line)
  {
    std::lock_guard<std::mutex> lock(logging_mutex);
    if (logging_config.error_log_stream)
    {
      write_line(*logging_config.error_log_stream, line);
      return;
    }

    write_line(std::cout, line);
  }

  void write_error_line(const std::string &line)
  {
    std::lock_guard<std::mutex> lock(logging_mutex);
    if (logging_config.error_log_stream)
    {
      write_line(*logging_config.error_log_stream, line);
      return;
    }

    write_line(std::cerr, line);
  }

  void write_access_line(const std::string &line)
  {
    std::lock_guard<std::mutex> lock(logging_mutex);
    if (logging_config.access_log_stream)
    {
      write_line(*logging_config.access_log_stream, line);
      return;
    }

    write_line(std::cout, line);
  }
}

void Vajra::runtime::configure_runtime_logging(
    bool structured_logs,
    const std::string &access_log,
    const std::string &error_log)
{
  std::string warning_message;
  std::lock_guard<std::mutex> lock(logging_mutex);
  logging_config.structured_logs = structured_logs;
  logging_config.access_log_path = access_log;
  logging_config.error_log_path = error_log;
  logging_config.access_log_stream.reset();
  logging_config.error_log_stream.reset();

  if (!access_log.empty())
  {
    auto access_stream = std::make_unique<std::ofstream>(access_log, std::ios::app);
    if (access_stream->is_open() && access_stream->good())
    {
      logging_config.access_log_stream = std::move(access_stream);
    }
    else
    {
      warning_message = "unable to open access_log at " + escaped_log_value(access_log) + "; falling back to stdout";
    }
  }
  if (!error_log.empty())
  {
    auto error_stream = std::make_unique<std::ofstream>(error_log, std::ios::app);
    if (error_stream->is_open() && error_stream->good())
    {
      logging_config.error_log_stream = std::move(error_stream);
    }
    else
    {
      if (!warning_message.empty())
      {
        warning_message.append("; ");
      }
      warning_message.append(
          "unable to open error_log at " + escaped_log_value(error_log) + "; falling back to stderr");
    }
  }

  if (!warning_message.empty())
  {
    write_line(std::cerr, "[Vajra][error] " + utc_timestamp() + ' ' + warning_message);
  }
}

void Vajra::runtime::configure_runtime_tracing(
    bool trace_enabled,
    const std::string &trace_endpoint,
    const std::string &trace_service_name)
{
  const std::lock_guard<std::mutex> lock(logging_mutex);
  logging_config.trace_enabled = trace_enabled;
  logging_config.trace_endpoint = trace_endpoint;
  logging_config.trace_service_name = trace_service_name;
  if (!trace_enabled)
  {
    logging_config.trace_available = false;
  }
}

void Vajra::runtime::set_runtime_tracing_available(bool available)
{
  const std::lock_guard<std::mutex> lock(logging_mutex);
  logging_config.trace_available = available;
}

bool Vajra::runtime::runtime_tracing_enabled()
{
  const std::lock_guard<std::mutex> lock(logging_mutex);
  return logging_config.trace_enabled;
}

bool Vajra::runtime::runtime_tracing_available()
{
  const std::lock_guard<std::mutex> lock(logging_mutex);
  return logging_config.trace_available;
}

std::string Vajra::runtime::runtime_tracing_endpoint()
{
  const std::lock_guard<std::mutex> lock(logging_mutex);
  return logging_config.trace_endpoint;
}

std::string Vajra::runtime::runtime_tracing_service_name()
{
  const std::lock_guard<std::mutex> lock(logging_mutex);
  return logging_config.trace_service_name;
}

void Vajra::runtime::set_runtime_lifecycle_callback(void *callback)
{
#ifdef VAJRA_RUNTIME_HAS_RUBY
  static bool callback_rooted = false;
  if (!callback_rooted)
  {
    rb_global_variable(&runtime_lifecycle_callback);
    callback_rooted = true;
  }
  const std::lock_guard<std::mutex> lock(logging_mutex);
  runtime_lifecycle_callback = reinterpret_cast<VALUE>(callback);
#else
  (void)callback;
#endif
}

void Vajra::runtime::flush_runtime_logs()
{
  flush_runtime_streams();
}

void Vajra::runtime::log_runtime_banner_start(
    const std::string &host,
    int port,
    int workers,
    std::size_t min_threads,
    std::size_t max_threads)
{
  const pid_t pid = getpid();
  std::ostringstream line;
  line << "[" << pid << "] === vajra boot: " << utc_timestamp() << " ===";
  write_runtime_line(line.str());
  write_runtime_line("[" + std::to_string(pid) + "] Vajra starting in master-worker mode...");
  write_runtime_line("[" + std::to_string(pid) + "] * Environment: " + runtime_environment_name());
  write_runtime_line("[" + std::to_string(pid) + "] * Master PID: " + std::to_string(pid));
  write_runtime_line("[" + std::to_string(pid) + "] * Workers: " + std::to_string(workers));
  write_runtime_line("[" + std::to_string(pid) + "] * Min threads: " + std::to_string(min_threads));
  write_runtime_line("[" + std::to_string(pid) + "] * Max threads: " + std::to_string(max_threads));
  write_runtime_line("[" + std::to_string(pid) + "] * Bind: http://" + host + ":" + std::to_string(port));
}

void Vajra::runtime::log_worker_lifecycle_event(
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
    int exit_detail)
{
  if (structured_logging_enabled())
  {
    std::ostringstream line;
    line << "{\"component\":\"lifecycle\""
         << ",\"timestamp\":\"" << utc_timestamp() << "\""
         << ",\"event\":" << escaped_log_value(event_name)
         << ",\"worker_index\":" << worker_index
         << ",\"pid\":" << pid
         << ",\"lifecycle\":\"" << worker_lifecycle_state_name(lifecycle_state) << "\""
         << ",\"health_state\":\"" << worker_health_state_name(health_state) << "\""
         << ",\"recovery_state\":\"" << worker_recovery_state_name(recovery_state) << "\""
         << ",\"availability\":\"" << (available ? "available" : "unavailable") << "\""
         << ",\"exit_classification\":\"" << worker_exit_classification_name(exit_classification) << "\""
         << ",\"terminal_replacement_failure\":" << (terminal_replacement_failure ? "true" : "false")
         << ",\"tracing_enabled\":" << (runtime_tracing_enabled() ? "true" : "false")
         << ",\"tracing_available\":" << (runtime_tracing_available() ? "true" : "false")
         << ",\"replacement_needed\":" << (replacement_needed ? "true" : "false");
    if (exit_detail != 0)
    {
      line << ",\"exit_detail\":" << exit_detail;
    }
    line << '}';
    write_runtime_line(line.str());
    flush_runtime_streams();
    emit_runtime_lifecycle_callback(
        event_name,
        worker_index,
        pid,
        lifecycle_state,
        health_state,
        recovery_state,
        available,
        exit_classification,
        terminal_replacement_failure,
        replacement_needed,
        exit_detail);
    return;
  }

  std::ostringstream line;
  line << "[Vajra][lifecycle] " << utc_timestamp()
       << " event=" << event_name
       << " worker_index=" << worker_index
       << " pid=" << pid
       << " lifecycle=" << worker_lifecycle_state_name(lifecycle_state)
       << " health_state=" << worker_health_state_name(health_state)
       << " recovery_state=" << worker_recovery_state_name(recovery_state)
       << " availability=" << (available ? "available" : "unavailable")
       << " exit_classification=" << worker_exit_classification_name(exit_classification)
       << " terminal_replacement_failure=" << (terminal_replacement_failure ? "true" : "false")
       << " tracing_enabled=" << (runtime_tracing_enabled() ? "true" : "false")
       << " tracing_available=" << (runtime_tracing_available() ? "true" : "false")
       << " replacement_needed=" << (replacement_needed ? "true" : "false");
  if (exit_detail != 0)
  {
    line << " exit_detail=" << exit_detail;
  }
  write_runtime_line(line.str());
  flush_runtime_streams();
  emit_runtime_lifecycle_callback(
      event_name,
      worker_index,
      pid,
      lifecycle_state,
      health_state,
      recovery_state,
      available,
      exit_classification,
      terminal_replacement_failure,
      replacement_needed,
      exit_detail);
}

void Vajra::runtime::log_unexpected_worker_exit(
    WorkerExitClassification exit_classification,
    int exit_detail)
{
  switch (exit_classification)
  {
    case WorkerExitClassification::unexpected_status:
      write_runtime_line("worker process exited unexpectedly with status " + std::to_string(exit_detail));
      flush_runtime_streams();
      return;
    case WorkerExitClassification::unexpected_signal:
      write_runtime_line("worker process exited unexpectedly due to signal " + std::to_string(exit_detail));
      flush_runtime_streams();
      return;
    case WorkerExitClassification::unexpected_exit:
      write_runtime_line("worker process exited unexpectedly");
      flush_runtime_streams();
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
  std::ostringstream line;
  line << "[Vajra][lifecycle] " << utc_timestamp()
       << " event=worker_bootstrap_ready state=booting boot_status=ready stop_reason=none"
       << " port=" << port
       << " listener_owned=false listener_fd=-1"
       << " mode=master_worker"
       << " process_role=" << runtime_role
       << " request_execution_role=" << runtime_role
       << " worker_processes=" << worker_processes;
  write_runtime_line(line.str());
  flush_runtime_streams();
}

void Vajra::runtime::log_worker_booted(int worker_index, pid_t pid, double boot_seconds)
{
  std::ostringstream message;
  message << "[" << getppid() << "] - Worker " << worker_index
          << " (PID: " << pid << ") booted in "
          << std::fixed << std::setprecision(2) << boot_seconds << "s";
  write_runtime_line(message.str());
  flush_runtime_streams();
}

void Vajra::runtime::log_runtime_error(const std::string &message)
{
  if (structured_logging_enabled())
  {
    write_error_line(
        "{\"component\":\"error\",\"timestamp\":\"" + utc_timestamp() + "\",\"message\":" +
        escaped_log_value(message) + "}");
    flush_runtime_streams();
    return;
  }

  write_error_line("[Vajra][error] " + utc_timestamp() + ' ' + message);
  flush_runtime_streams();
}

void Vajra::runtime::log_access_event(const std::string &method, const std::string &target, int status_code)
{
  if (structured_logging_enabled())
  {
    std::ostringstream line;
    line << "{\"component\":\"access\""
         << ",\"timestamp\":\"" << utc_timestamp() << "\""
         << ",\"method\":" << escaped_log_value(method)
         << ",\"target\":" << escaped_log_value(target)
         << ",\"status\":" << status_code
         << '}';
    write_access_line(line.str());
    return;
  }

  std::ostringstream line;
  line << "[Vajra][access] " << utc_timestamp()
       << " method=" << escaped_log_value(method)
       << " target=" << escaped_log_value(target)
       << " status=" << status_code;
  write_access_line(line.str());
}

void Vajra::runtime::log_runtime_shutdown_begin()
{
  write_runtime_line("[" + std::to_string(getpid()) + "] - Gracefully shutting down workers...");
  flush_runtime_streams();
}

void Vajra::runtime::log_runtime_stop_completed()
{
  write_runtime_line("[Vajra][lifecycle] " + utc_timestamp() + " event=stop_completed");
  flush_runtime_streams();
}

void Vajra::runtime::log_runtime_shutdown_complete()
{
  write_runtime_line("[" + std::to_string(getpid()) + "] === vajra shutdown: " + utc_timestamp() + " ===");
  write_runtime_line("[" + std::to_string(getpid()) + "] - Goodbye!");
  flush_runtime_streams();
}
