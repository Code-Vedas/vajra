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
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <utility>
#include <vector>
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
    bool access_log_disabled = false;
    std::string access_log_path;
    std::string error_log_path;
    std::string trace_endpoint;
    std::string trace_service_name;
    int access_log_fd = STDOUT_FILENO;
    int error_log_fd = STDERR_FILENO;
    bool close_access_log_fd = false;
    bool close_error_log_fd = false;
  };

  enum class LogEventKind
  {
    runtime,
    error,
    access,
  };

  struct LogEvent
  {
    LogEventKind kind = LogEventKind::runtime;
    bool structured = false;
    std::string line;
    std::string method;
    std::string target;
    int status_code = 0;
  };

  struct LogNode
  {
    explicit LogNode(LogEvent event_payload) : event(std::move(event_payload)) {}

    std::atomic<LogNode *> next{nullptr};
    LogEvent event;
  };

  struct AsyncLoggerState
  {
    std::mutex mutex;
    std::condition_variable condition;
    std::condition_variable drained;
    std::thread worker;
    std::atomic_bool running{false};
    std::atomic_bool stopping{false};
    std::size_t in_flight = 0;
    std::atomic<pid_t> owner_pid{-1};
    std::atomic<std::size_t> pending{0};
    LogNode *head = nullptr;
    std::atomic<LogNode *> tail{nullptr};
    bool structured_logs = false;
    bool access_log_disabled = false;
    int access_log_fd = STDOUT_FILENO;
    int error_log_fd = STDERR_FILENO;
    int runtime_log_fd = STDOUT_FILENO;
  };

  std::mutex logging_mutex;
  LoggingConfig logging_config;
  AsyncLoggerState async_logger;
  std::atomic_bool access_log_disabled{false};
  std::atomic_bool structured_logs_enabled{false};
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

  bool write_all(int fd, const char *data, std::size_t length)
  {
    std::size_t written = 0;
    while (written < length)
    {
      const ssize_t result = ::write(fd, data + written, length - written);
      if (result < 0)
      {
        if (errno == EINTR)
        {
          continue;
        }
        return false;
      }
      if (result == 0)
      {
        return false;
      }
      written += static_cast<std::size_t>(result);
    }
    return true;
  }

  void write_line_fd(int fd, const std::string &line)
  {
    std::string payload;
    payload.reserve(line.size() + 1);
    payload.append(line);
    payload.push_back('\n');
    (void)write_all(fd, payload.data(), payload.size());
  }

  void append_line(std::string &buffer, const std::string &line)
  {
    buffer.append(line);
    buffer.push_back('\n');
  }

  int open_log_fd(const std::string &path)
  {
    return open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  }

  const std::string &cached_utc_timestamp()
  {
    thread_local std::time_t cached_time = 0;
    thread_local std::string cached_timestamp;
    const auto now = std::chrono::system_clock::now();
    const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    if (now_time != cached_time || cached_timestamp.empty())
    {
      std::tm utc_time{};
      gmtime_r(&now_time, &utc_time);
      std::ostringstream timestamp;
      timestamp << std::put_time(&utc_time, "%Y-%m-%dT%H:%M:%SZ");
      cached_time = now_time;
      cached_timestamp = timestamp.str();
    }
    return cached_timestamp;
  }

  void close_configured_fd(int &fd, bool &should_close, int fallback_fd)
  {
    if (should_close && fd >= 0)
    {
      close(fd);
    }
    fd = fallback_fd;
    should_close = false;
  }

  std::string formatted_access_line(const LogEvent &event)
  {
    if (event.structured)
    {
      std::ostringstream line;
      line << "{\"component\":\"access\""
           << ",\"timestamp\":\"" << cached_utc_timestamp() << "\""
           << ",\"method\":" << escaped_log_value(event.method)
           << ",\"target\":" << escaped_log_value(event.target)
           << ",\"status\":" << event.status_code
           << '}';
      return line.str();
    }

    std::ostringstream line;
    line << "[Vajra][access] " << cached_utc_timestamp()
         << " method=" << escaped_log_value(event.method)
         << " target=" << escaped_log_value(event.target)
         << " status=" << event.status_code;
    return line.str();
  }

  bool async_logger_owned_by_current_process()
  {
    return async_logger.running.load(std::memory_order_acquire) &&
           async_logger.owner_pid.load(std::memory_order_acquire) == getpid();
  }

  void write_log_event(const LogEvent &event)
  {
    switch (event.kind)
    {
      case LogEventKind::runtime:
        write_line_fd(async_logger.runtime_log_fd, event.line);
        return;
      case LogEventKind::error:
        write_line_fd(async_logger.error_log_fd, event.line);
        return;
      case LogEventKind::access:
        if (!async_logger.access_log_disabled)
        {
          write_line_fd(async_logger.access_log_fd, event.line.empty() ? formatted_access_line(event) : event.line);
        }
        return;
    }
  }

  void append_log_event(const LogEvent &event, std::string &runtime_buffer, std::string &error_buffer, std::string &access_buffer)
  {
    switch (event.kind)
    {
      case LogEventKind::runtime:
        append_line(runtime_buffer, event.line);
        return;
      case LogEventKind::error:
        append_line(error_buffer, event.line);
        return;
      case LogEventKind::access:
        if (!async_logger.access_log_disabled)
        {
          append_line(access_buffer, event.line.empty() ? formatted_access_line(event) : event.line);
        }
        return;
    }
  }

  void write_log_batch(const std::vector<LogEvent> &events)
  {
    const bool access_sink_enabled = !async_logger.access_log_disabled;
    const bool shared_sink = async_logger.runtime_log_fd == async_logger.error_log_fd ||
                             (access_sink_enabled &&
                              (async_logger.access_log_fd == async_logger.runtime_log_fd ||
                               async_logger.access_log_fd == async_logger.error_log_fd));
    if (shared_sink)
    {
      for (const LogEvent &event : events)
      {
        write_log_event(event);
      }
      return;
    }

    std::string runtime_buffer;
    std::string error_buffer;
    std::string access_buffer;
    runtime_buffer.reserve(4096);
    error_buffer.reserve(4096);
    access_buffer.reserve(events.size() * 96);

    for (const LogEvent &event : events)
    {
      append_log_event(event, runtime_buffer, error_buffer, access_buffer);
    }

    if (!runtime_buffer.empty())
    {
      (void)write_all(async_logger.runtime_log_fd, runtime_buffer.data(), runtime_buffer.size());
    }
    if (!error_buffer.empty())
    {
      (void)write_all(async_logger.error_log_fd, error_buffer.data(), error_buffer.size());
    }
    if (!access_buffer.empty())
    {
      (void)write_all(async_logger.access_log_fd, access_buffer.data(), access_buffer.size());
    }
  }

  void async_logger_loop()
  {
    for (;;)
    {
      std::vector<LogEvent> events;
      for (;;)
      {
        LogNode *head = async_logger.head;
        LogNode *next = head == nullptr ? nullptr : head->next.load(std::memory_order_acquire);
        if (next == nullptr)
        {
          break;
        }
        events.push_back(std::move(next->event));
        async_logger.head = next;
        delete head;
      }

      if (events.empty())
      {
        if (async_logger.stopping.load(std::memory_order_acquire) &&
            async_logger.pending.load(std::memory_order_acquire) == 0)
        {
          async_logger.drained.notify_all();
          return;
        }

        std::unique_lock<std::mutex> lock(async_logger.mutex);
        async_logger.condition.wait_for(lock, std::chrono::milliseconds(1), [] {
          return async_logger.stopping.load(std::memory_order_acquire) ||
                 async_logger.pending.load(std::memory_order_acquire) > 0;
        });
        continue;
      }

      {
        std::lock_guard<std::mutex> lock(async_logger.mutex);
        async_logger.in_flight += events.size();
        async_logger.pending.fetch_sub(events.size(), std::memory_order_acq_rel);
      }

      write_log_batch(events);

      {
        std::lock_guard<std::mutex> lock(async_logger.mutex);
        async_logger.in_flight -= events.size();
        if (async_logger.pending.load(std::memory_order_acquire) == 0 && async_logger.in_flight == 0)
        {
          async_logger.drained.notify_all();
        }
      }
    }
  }

  void delete_async_logger_queue()
  {
    LogNode *node = async_logger.head;
    while (node != nullptr)
    {
      LogNode *next = node->next.load(std::memory_order_acquire);
      delete node;
      node = next;
    }
    async_logger.head = nullptr;
    async_logger.tail.store(nullptr, std::memory_order_release);
  }

  bool enqueue_log_event(LogEvent event)
  {
    if (!async_logger_owned_by_current_process() || async_logger.stopping.load(std::memory_order_acquire))
    {
      return false;
    }
    auto *node = new LogNode(std::move(event));
    const bool should_notify = async_logger.pending.fetch_add(1, std::memory_order_acq_rel) == 0;
    LogNode *previous = async_logger.tail.exchange(node, std::memory_order_acq_rel);
    previous->next.store(node, std::memory_order_release);
    if (should_notify)
    {
      async_logger.condition.notify_one();
    }
    return true;
  }

  void drain_async_logger()
  {
    std::unique_lock<std::mutex> lock(async_logger.mutex);
    if (!async_logger_owned_by_current_process())
    {
      return;
    }
    async_logger.drained.wait(lock, [] {
      return async_logger.pending.load(std::memory_order_acquire) == 0 && async_logger.in_flight == 0;
    });
  }

  void flush_runtime_streams()
  {
    drain_async_logger();
    std::cout.flush();
    std::cerr.flush();
  }

  bool structured_logging_enabled()
  {
    return structured_logs_enabled.load(std::memory_order_acquire);
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
    if (enqueue_log_event(LogEvent{LogEventKind::runtime, false, line, "", "", 0}))
    {
      return;
    }

    int fd = STDOUT_FILENO;
    {
      std::lock_guard<std::mutex> lock(logging_mutex);
      fd = logging_config.error_log_path.empty() ? STDOUT_FILENO : logging_config.error_log_fd;
    }
    write_line_fd(fd, line);
  }

  void write_error_line(const std::string &line)
  {
    if (enqueue_log_event(LogEvent{LogEventKind::error, false, line, "", "", 0}))
    {
      return;
    }

    int fd = STDERR_FILENO;
    {
      std::lock_guard<std::mutex> lock(logging_mutex);
      fd = logging_config.error_log_fd;
    }
    write_line_fd(fd, line);
  }

  void write_access_line(const std::string &line)
  {
    if (enqueue_log_event(LogEvent{LogEventKind::access, structured_logging_enabled(), line, "", "", 0}))
    {
      return;
    }

    int fd = STDOUT_FILENO;
    {
      std::lock_guard<std::mutex> lock(logging_mutex);
      if (logging_config.access_log_disabled)
      {
        return;
      }
      fd = logging_config.access_log_fd;
    }
    write_line_fd(fd, line);
  }
}

void Vajra::runtime::configure_runtime_logging(
    bool structured_logs,
    const std::string &access_log,
    const std::string &error_log)
{
  std::string warning_message;
  stop_runtime_logging_worker();
  std::lock_guard<std::mutex> lock(logging_mutex);
  close_configured_fd(logging_config.access_log_fd, logging_config.close_access_log_fd, STDOUT_FILENO);
  close_configured_fd(logging_config.error_log_fd, logging_config.close_error_log_fd, STDERR_FILENO);
  logging_config.structured_logs = structured_logs;
  logging_config.access_log_path = access_log;
  logging_config.error_log_path = error_log;
  logging_config.access_log_disabled = access_log.empty() || access_log == "/dev/null";
  access_log_disabled.store(logging_config.access_log_disabled, std::memory_order_release);
  structured_logs_enabled.store(structured_logs, std::memory_order_release);

  if (!access_log.empty() && !logging_config.access_log_disabled)
  {
    const int access_fd = open_log_fd(access_log);
    if (access_fd >= 0)
    {
      logging_config.access_log_fd = access_fd;
      logging_config.close_access_log_fd = true;
    }
    else
    {
      warning_message = "unable to open access_log at " + escaped_log_value(access_log) + "; falling back to stdout";
    }
  }
  if (!error_log.empty())
  {
    const int error_fd = open_log_fd(error_log);
    if (error_fd >= 0)
    {
      logging_config.error_log_fd = error_fd;
      logging_config.close_error_log_fd = true;
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
    write_line_fd(STDERR_FILENO, "[Vajra][error] " + utc_timestamp() + ' ' + warning_message);
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

void Vajra::runtime::start_runtime_logging_worker()
{
  LoggingConfig config_snapshot;
  {
    const std::lock_guard<std::mutex> lock(logging_mutex);
    config_snapshot.structured_logs = logging_config.structured_logs;
    config_snapshot.access_log_disabled = logging_config.access_log_disabled;
    config_snapshot.access_log_path = logging_config.access_log_path;
    config_snapshot.error_log_path = logging_config.error_log_path;
    config_snapshot.access_log_fd = logging_config.access_log_fd;
    config_snapshot.error_log_fd = logging_config.error_log_fd;
  }

  std::lock_guard<std::mutex> lock(async_logger.mutex);
  if (async_logger.running.load(std::memory_order_acquire) &&
      async_logger.owner_pid.load(std::memory_order_acquire) == getpid())
  {
    return;
  }
  if (async_logger.running.load(std::memory_order_acquire) &&
      async_logger.owner_pid.load(std::memory_order_acquire) != getpid())
  {
    async_logger.running.store(false, std::memory_order_release);
    async_logger.stopping.store(false, std::memory_order_release);
    async_logger.in_flight = 0;
    async_logger.pending.store(0, std::memory_order_release);
    delete_async_logger_queue();
  }

  auto *stub = new LogNode(LogEvent{});
  async_logger.head = stub;
  async_logger.tail.store(stub, std::memory_order_release);
  async_logger.pending.store(0, std::memory_order_release);
  async_logger.structured_logs = config_snapshot.structured_logs;
  async_logger.access_log_disabled = config_snapshot.access_log_disabled;
  async_logger.access_log_fd = config_snapshot.access_log_fd;
  async_logger.error_log_fd = config_snapshot.error_log_fd;
  async_logger.runtime_log_fd = config_snapshot.error_log_path.empty() ? STDOUT_FILENO : config_snapshot.error_log_fd;
  async_logger.owner_pid.store(getpid(), std::memory_order_release);
  async_logger.stopping.store(false, std::memory_order_release);
  async_logger.running.store(true, std::memory_order_release);
  async_logger.worker = std::thread(async_logger_loop);
}

void Vajra::runtime::stop_runtime_logging_worker()
{
  std::thread worker;
  {
    std::lock_guard<std::mutex> lock(async_logger.mutex);
    if (!async_logger_owned_by_current_process())
    {
      return;
    }
    async_logger.stopping.store(true, std::memory_order_release);
    async_logger.condition.notify_all();
    worker = std::move(async_logger.worker);
  }

  if (worker.joinable())
  {
    worker.join();
  }

  {
    std::lock_guard<std::mutex> lock(async_logger.mutex);
    async_logger.running.store(false, std::memory_order_release);
    async_logger.stopping.store(false, std::memory_order_release);
    async_logger.in_flight = 0;
    async_logger.pending.store(0, std::memory_order_release);
    delete_async_logger_queue();
    async_logger.owner_pid.store(-1, std::memory_order_release);
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
  write_runtime_line("[" + std::to_string(pid) + "] Use Ctrl-C to stop");
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
  if (access_log_disabled.load(std::memory_order_acquire))
  {
    return;
  }

  if (enqueue_log_event(LogEvent{
          LogEventKind::access,
          structured_logs_enabled.load(std::memory_order_acquire),
          "",
          method,
          target,
          status_code}))
  {
    return;
  }

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
    flush_runtime_streams();
    return;
  }

  std::ostringstream line;
  line << "[Vajra][access] " << utc_timestamp()
       << " method=" << escaped_log_value(method)
       << " target=" << escaped_log_value(target)
       << " status=" << status_code;
  write_access_line(line.str());
  flush_runtime_streams();
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
