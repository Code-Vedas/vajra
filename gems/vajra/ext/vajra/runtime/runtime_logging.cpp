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

#include <algorithm>
#include <chrono>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <netdb.h>
#include <optional>
#include <signal.h>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/socket.h>
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
    bool trace_active_context_required = false;
    double trace_sample_ratio = 1.0;
    bool access_log_disabled = false;
    std::string access_log_format;
    std::string access_log_path;
    std::string error_log_path;
    std::string trace_endpoint;
    std::string trace_service_name;
    Vajra::runtime::AccessLogFieldNeeds access_field_needs;
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
    Vajra::runtime::AccessLogEvent access;
  };

  struct LogNode
  {
    std::atomic<LogNode *> next{nullptr};
    LogEvent event;
  };

  struct LogNodeBlock
  {
    explicit LogNodeBlock(std::size_t count) : nodes(new LogNode[count]), size(count) {}

    std::unique_ptr<LogNode[]> nodes;
    std::size_t size = 0;
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
    std::atomic<LogNode *> free_head{nullptr};
    std::mutex pool_mutex;
    std::vector<LogNodeBlock> blocks;
    std::size_t next_block_size = 4096;
    bool structured_logs = false;
    bool access_log_disabled = false;
    std::string access_log_format;
    int access_log_fd = STDOUT_FILENO;
    int error_log_fd = STDERR_FILENO;
    int runtime_log_fd = STDOUT_FILENO;
  };

  std::mutex logging_mutex;
  LoggingConfig logging_config;
  AsyncLoggerState async_logger;
  std::atomic_bool access_log_disabled{false};
  std::atomic_bool structured_logs_enabled{false};
  std::atomic_bool reopen_requested{false};
  std::atomic_bool access_need_host{false};
  std::atomic_bool access_need_user_agent{false};
  std::atomic_bool access_need_referer{false};
  std::atomic_bool access_need_request_id{false};
  std::atomic_bool access_need_trace_context{false};
  std::atomic_bool trace_available_snapshot{false};
  std::atomic_bool trace_active_context_required_snapshot{false};
  std::atomic<std::uint64_t> trace_sample_threshold{std::numeric_limits<std::uint64_t>::max()};
  std::atomic<std::uint64_t> trace_sample_counter{0};
  std::atomic_bool request_observability_enabled{false};
  std::mutex request_observability_mutex;
  std::deque<Vajra::runtime::RequestObservabilityEvent> request_observability_events;
  std::deque<Vajra::runtime::RequestSpanEvent> native_otlp_span_events;
  std::condition_variable request_span_condition;
  std::atomic<std::uint64_t> native_request_observability_events_total{0};
  std::atomic<std::uint64_t> native_request_observability_errors_total{0};
  std::atomic<std::uint64_t> native_request_admission_rejections_total{0};
  constexpr std::size_t kNativeOtlpBatchSize = 4096;
  constexpr auto kNativeOtlpFlushInterval = std::chrono::milliseconds(25);

  struct NativeOtlpEndpoint
  {
    std::string host;
    std::string host_header;
    std::string port;
    std::string path;
  };

  struct NativeOtlpExporterState
  {
    std::mutex mutex;
    std::thread worker;
    std::atomic_bool running{false};
    std::atomic_bool stopping{false};
    std::atomic<pid_t> owner_pid{-1};
    NativeOtlpEndpoint endpoint;
    std::string service_name;
  };

  NativeOtlpExporterState native_otlp_exporter;
  std::atomic_bool native_otlp_export_enabled{false};
  std::atomic<std::uint64_t> native_otlp_id_counter{0};

  std::string traceparent_part(const std::string &traceparent, int part)
  {
    std::size_t cursor = 0;
    for (int index = 0; index < part; ++index)
    {
      cursor = traceparent.find('-', cursor);
      if (cursor == std::string::npos)
      {
        return "";
      }
      ++cursor;
    }
    const std::size_t end = traceparent.find('-', cursor);
    return traceparent.substr(cursor, end == std::string::npos ? std::string::npos : end - cursor);
  }

  std::uint64_t trace_sample_threshold_for(double ratio)
  {
    if (ratio <= 0.0)
    {
      return 0;
    }
    if (ratio >= 1.0)
    {
      return std::numeric_limits<std::uint64_t>::max();
    }
    return static_cast<std::uint64_t>(ratio * static_cast<double>(std::numeric_limits<std::uint64_t>::max()));
  }

  int traceparent_sample_flag(const std::string &traceparent)
  {
    const std::string flags = traceparent_part(traceparent, 3);
    if (flags.size() != 2)
    {
      return -1;
    }
    char *end = nullptr;
    const long value = std::strtol(flags.c_str(), &end, 16);
    if (end == flags.c_str() || *end != '\0')
    {
      return -1;
    }
    return (value & 0x01) == 0x01 ? 1 : 0;
  }

  std::uint64_t sampled_hash_value(std::uint64_t value)
  {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
  }
#ifdef VAJRA_RUNTIME_HAS_RUBY
  VALUE runtime_lifecycle_callback = Qnil;
  VALUE runtime_request_observability_callback = Qnil;

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

  std::string escaped_access_token_value(const std::string &value)
  {
    std::ostringstream escaped;
    for (unsigned char character : value)
    {
      switch (character)
      {
        case '\\':
          escaped << "\\\\";
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

  void prepare_socket_write(int fd)
  {
#ifdef SO_NOSIGPIPE
    int opt = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &opt, sizeof(opt));
#else
    (void)fd;
#endif
  }

  bool send_all(int fd, const char *data, std::size_t length)
  {
#ifdef MSG_NOSIGNAL
    constexpr int send_flags = MSG_NOSIGNAL;
#else
    constexpr int send_flags = 0;
#endif
    std::size_t sent_total = 0;
    while (sent_total < length)
    {
      const ssize_t result = ::send(fd, data + sent_total, length - sent_total, send_flags);
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
      sent_total += static_cast<std::size_t>(result);
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

  std::string dash_if_empty(const std::string &value)
  {
    return value.empty() ? "-" : value;
  }

  std::string access_format()
  {
    if (!async_logger.access_log_format.empty())
    {
      return async_logger.access_log_format;
    }
    return async_logger.structured_logs ? "json" : "text";
  }

  void require_token_fields(Vajra::runtime::AccessLogFieldNeeds &needs, char token)
  {
    switch (token)
    {
      case 'h':
        needs.host = true;
        return;
      case 'u':
        needs.user_agent = true;
        return;
      case 'r':
        needs.referer = true;
        return;
      case 'i':
        needs.request_id = true;
        return;
      case 'T':
      case 'S':
        needs.trace_context = true;
        return;
      default:
        return;
    }
  }

  Vajra::runtime::AccessLogFieldNeeds field_needs_for_format(const std::string &format, bool structured_logs)
  {
    Vajra::runtime::AccessLogFieldNeeds needs;
    const std::string effective_format = format.empty() ? (structured_logs ? "json" : "text") : format;
    if (effective_format == "json" || effective_format == "text")
    {
      needs.host = true;
      needs.user_agent = true;
      needs.referer = true;
      needs.request_id = true;
      needs.trace_context = true;
      return needs;
    }
    if (effective_format == "combined")
    {
      needs.user_agent = true;
      needs.referer = true;
      return needs;
    }
    if (effective_format == "common")
    {
      return needs;
    }
    for (std::size_t index = 0; index < effective_format.size(); ++index)
    {
      if (effective_format[index] == '%' && index + 1 < effective_format.size())
      {
        require_token_fields(needs, effective_format[++index]);
      }
    }
    return needs;
  }

  void publish_access_field_needs(const Vajra::runtime::AccessLogFieldNeeds &needs)
  {
    access_need_host.store(needs.host, std::memory_order_release);
    access_need_user_agent.store(needs.user_agent, std::memory_order_release);
    access_need_referer.store(needs.referer, std::memory_order_release);
    access_need_request_id.store(needs.request_id, std::memory_order_release);
    access_need_trace_context.store(needs.trace_context, std::memory_order_release);
  }

  void signal_reopen_handler(int)
  {
    reopen_requested.store(true, std::memory_order_release);
  }

  void install_reopen_signal_handler()
  {
    struct sigaction action {};
    action.sa_handler = signal_reopen_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    (void)sigaction(SIGUSR1, &action, nullptr);
  }

  bool reopen_configured_logs_locked(std::string &warning_message)
  {
    int next_access_fd = STDOUT_FILENO;
    bool next_close_access_fd = false;
    int next_error_fd = STDERR_FILENO;
    bool next_close_error_fd = false;

    if (!logging_config.access_log_path.empty() && !logging_config.access_log_disabled)
    {
      next_access_fd = open_log_fd(logging_config.access_log_path);
      if (next_access_fd >= 0)
      {
        next_close_access_fd = true;
      }
      else
      {
        warning_message = "unable to reopen access_log at " + escaped_log_value(logging_config.access_log_path) +
                          "; keeping previous access log sink";
        next_access_fd = logging_config.access_log_fd;
      }
    }

    if (!logging_config.error_log_path.empty())
    {
      next_error_fd = open_log_fd(logging_config.error_log_path);
      if (next_error_fd >= 0)
      {
        next_close_error_fd = true;
      }
      else
      {
        if (!warning_message.empty())
        {
          warning_message.append("; ");
        }
        warning_message.append("unable to reopen error_log at " + escaped_log_value(logging_config.error_log_path) +
                               "; keeping previous error log sink");
        next_error_fd = logging_config.error_log_fd;
      }
    }

    if (next_close_access_fd)
    {
      close_configured_fd(logging_config.access_log_fd, logging_config.close_access_log_fd, STDOUT_FILENO);
      logging_config.access_log_fd = next_access_fd;
      logging_config.close_access_log_fd = true;
    }
    if (next_close_error_fd)
    {
      close_configured_fd(logging_config.error_log_fd, logging_config.close_error_log_fd, STDERR_FILENO);
      logging_config.error_log_fd = next_error_fd;
      logging_config.close_error_log_fd = true;
    }
    return next_close_access_fd || next_close_error_fd;
  }

  void service_reopen_signal_if_requested()
  {
    if (!reopen_requested.exchange(false, std::memory_order_acq_rel))
    {
      return;
    }

    std::string warning_message;
    {
      std::lock_guard<std::mutex> lock(logging_mutex);
      if (reopen_configured_logs_locked(warning_message))
      {
        async_logger.access_log_fd = logging_config.access_log_fd;
        async_logger.error_log_fd = logging_config.error_log_fd;
        async_logger.runtime_log_fd = logging_config.error_log_path.empty() ? STDOUT_FILENO : logging_config.error_log_fd;
      }
    }
    if (!warning_message.empty())
    {
      write_line_fd(STDERR_FILENO, "[Vajra][error] " + Vajra::runtime::utc_timestamp() + ' ' + warning_message);
    }
  }

  std::string common_access_line(const Vajra::runtime::AccessLogEvent &event, bool combined)
  {
    std::ostringstream line;
    line << dash_if_empty(event.remote_address)
         << " - - [" << cached_utc_timestamp() << "] "
         << escaped_log_value(event.method + " " + event.target + " " + dash_if_empty(event.protocol))
         << ' ' << event.status_code
         << ' ' << event.response_body_bytes;
    if (combined)
    {
      line << ' ' << escaped_log_value(dash_if_empty(event.referer))
           << ' ' << escaped_log_value(dash_if_empty(event.user_agent));
    }
    return line.str();
  }

  std::string token_value(const Vajra::runtime::AccessLogEvent &event, char token)
  {
    switch (token)
    {
      case 'm': return event.method;
      case 'U': return event.target;
      case 's': return std::to_string(event.status_code);
      case 'b': return std::to_string(event.response_body_bytes);
      case 'a': return event.remote_address;
      case 'H': return event.protocol;
      case 'h': return event.host;
      case 'u': return event.user_agent;
      case 'r': return event.referer;
      case 'i': return event.request_id;
      case 'D': return std::to_string(event.duration_nanoseconds);
      case 'p': return std::to_string(event.worker_pid);
      case 'w': return std::to_string(event.worker_index);
      case 'c': return event.connection_outcome;
      case 'T': return event.trace_id;
      case 'S': return event.span_id;
      default: return std::string(1, token);
    }
  }

  std::string custom_access_line(const Vajra::runtime::AccessLogEvent &event, const std::string &format)
  {
    std::string line;
    line.reserve(format.size() + 64);
    for (std::size_t index = 0; index < format.size(); ++index)
    {
      if (format[index] == '%' && index + 1 < format.size())
      {
        line.append(escaped_access_token_value(token_value(event, format[++index])));
      }
      else
      {
        line.push_back(format[index]);
      }
    }
    return line;
  }

  void append_json_string_field(std::string &line, const char *name, const std::string &value)
  {
    line.push_back(',');
    line.push_back('"');
    line.append(name);
    line.append("\":");
    line.append(escaped_log_value(value));
  }

  void append_json_integer_field(std::string &line, const char *name, long long value)
  {
    line.push_back(',');
    line.push_back('"');
    line.append(name);
    line.append("\":");
    line.append(std::to_string(value));
  }

  std::string json_access_line(const Vajra::runtime::AccessLogEvent &event)
  {
    std::string line;
    line.reserve(320 + event.method.size() + event.target.size() + event.user_agent.size() + event.referer.size());
    line.append("{\"component\":\"access\"");
    append_json_string_field(line, "timestamp", cached_utc_timestamp());
    append_json_string_field(line, "method", event.method);
    append_json_string_field(line, "target", event.target);
    append_json_integer_field(line, "status", event.status_code);
    append_json_integer_field(line, "duration_nanoseconds", event.duration_nanoseconds);
    append_json_integer_field(line, "bytes_written", static_cast<long long>(event.response_body_bytes));
    append_json_string_field(line, "remote_addr", event.remote_address);
    append_json_string_field(line, "protocol", event.protocol);
    append_json_string_field(line, "host", event.host);
    append_json_string_field(line, "user_agent", event.user_agent);
    append_json_string_field(line, "referer", event.referer);
    append_json_string_field(line, "request_id", event.request_id);
    append_json_integer_field(line, "worker_pid", event.worker_pid);
    append_json_integer_field(line, "worker_index", event.worker_index);
    append_json_string_field(line, "connection_outcome", event.connection_outcome);
    if (!event.trace_id.empty())
    {
      append_json_string_field(line, "trace_id", event.trace_id);
    }
    if (!event.span_id.empty())
    {
      append_json_string_field(line, "span_id", event.span_id);
    }
    line.push_back('}');
    return line;
  }

  std::string formatted_access_line(const LogEvent &event)
  {
    const std::string format = access_format();
    if (format == "json")
    {
      return json_access_line(event.access);
    }
    if (format == "common")
    {
      return common_access_line(event.access, false);
    }
    if (format == "combined")
    {
      return common_access_line(event.access, true);
    }
    if (!format.empty() && format != "text")
    {
      return custom_access_line(event.access, format);
    }

    std::ostringstream line;
    line << "[Vajra][access] " << cached_utc_timestamp()
         << " method=" << escaped_log_value(event.access.method)
         << " target=" << escaped_log_value(event.access.target)
         << " status=" << event.access.status_code
         << " duration_ns=" << event.access.duration_nanoseconds
         << " bytes=" << event.access.response_body_bytes
         << " remote_addr=" << escaped_log_value(event.access.remote_address)
         << " protocol=" << escaped_log_value(event.access.protocol)
         << " host=" << escaped_log_value(event.access.host)
         << " user_agent=" << escaped_log_value(event.access.user_agent)
         << " referer=" << escaped_log_value(event.access.referer)
         << " request_id=" << escaped_log_value(event.access.request_id)
         << " worker_pid=" << event.access.worker_pid
         << " worker_index=" << event.access.worker_index
         << " connection=" << escaped_log_value(event.access.connection_outcome);
    if (!event.access.trace_id.empty())
    {
      line << " trace_id=" << escaped_log_value(event.access.trace_id);
    }
    if (!event.access.span_id.empty())
    {
      line << " span_id=" << escaped_log_value(event.access.span_id);
    }
    return line.str();
  }

  bool async_logger_owned_by_current_process()
  {
    return async_logger.running.load(std::memory_order_acquire) &&
           async_logger.owner_pid.load(std::memory_order_acquire) == getpid();
  }

  void release_log_node(LogNode *node)
  {
    if (node == nullptr)
    {
      return;
    }
    node->event = LogEvent{};
    LogNode *head = async_logger.free_head.load(std::memory_order_acquire);
    do
    {
      node->next.store(head, std::memory_order_relaxed);
    } while (!async_logger.free_head.compare_exchange_weak(
        head,
        node,
        std::memory_order_release,
        std::memory_order_acquire));
  }

  void add_log_node_block_locked(std::size_t count)
  {
    async_logger.blocks.emplace_back(count);
    LogNodeBlock &block = async_logger.blocks.back();
    for (std::size_t index = 0; index < block.size; ++index)
    {
      release_log_node(&block.nodes[index]);
    }
    async_logger.next_block_size = std::min<std::size_t>(async_logger.next_block_size * 2, 65'536);
  }

  LogNode *acquire_log_node()
  {
    for (;;)
    {
      LogNode *head = async_logger.free_head.load(std::memory_order_acquire);
      while (head != nullptr)
      {
        LogNode *next = head->next.load(std::memory_order_relaxed);
        if (async_logger.free_head.compare_exchange_weak(
                head,
                next,
                std::memory_order_acq_rel,
                std::memory_order_acquire))
        {
          head->next.store(nullptr, std::memory_order_relaxed);
          return head;
        }
      }

      {
        std::lock_guard<std::mutex> lock(async_logger.pool_mutex);
        if (async_logger.free_head.load(std::memory_order_acquire) == nullptr)
        {
          add_log_node_block_locked(async_logger.next_block_size);
        }
      }
    }
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
        release_log_node(head);
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
      release_log_node(node);
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
    LogNode *node = acquire_log_node();
    if (node == nullptr)
    {
      return false;
    }
    node->event = std::move(event);
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

  bool native_request_observability_success(
      const Vajra::runtime::AccessLogEvent &event,
      const std::string &failure_kind,
      bool response_sent)
  {
    return response_sent && event.status_code > 0 && event.status_code < 500 && failure_kind.empty();
  }

  bool native_request_admission_outcome(const std::string &outcome)
  {
    return outcome == "queue_capacity" || outcome == "request_timeout";
  }

  bool native_request_span_success(const Vajra::runtime::RequestSpanEvent &event)
  {
    return event.response_sent && event.status_code > 0 && event.status_code < 500 && event.failure_kind.empty();
  }

  std::optional<NativeOtlpEndpoint> parse_native_otlp_endpoint(const std::string &endpoint)
  {
    constexpr const char *prefix = "http://";
    if (endpoint.rfind(prefix, 0) != 0)
    {
      return std::nullopt;
    }
    const std::string rest = endpoint.substr(std::char_traits<char>::length(prefix));
    const std::size_t slash = rest.find('/');
    const std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
    if (authority.empty())
    {
      return std::nullopt;
    }
    NativeOtlpEndpoint parsed;
    parsed.host_header = authority;
    if (authority.front() == '[')
    {
      const std::size_t bracket = authority.find(']');
      if (bracket == std::string::npos || bracket == 1)
      {
        return std::nullopt;
      }
      parsed.host = authority.substr(1, bracket - 1);
      if (bracket + 1 == authority.size())
      {
        parsed.port = "80";
      }
      else if (authority[bracket + 1] == ':' && bracket + 2 < authority.size())
      {
        parsed.port = authority.substr(bracket + 2);
      }
      else
      {
        return std::nullopt;
      }
    }
    else
    {
      const std::size_t first_colon = authority.find(':');
      const std::size_t last_colon = authority.rfind(':');
      if (first_colon != last_colon)
      {
        return std::nullopt;
      }
      parsed.host = first_colon == std::string::npos ? authority : authority.substr(0, first_colon);
      parsed.port = first_colon == std::string::npos ? "80" : authority.substr(first_colon + 1);
    }
    parsed.path = slash == std::string::npos ? "/" : rest.substr(slash);
    if (parsed.host.empty() || parsed.port.empty())
    {
      return std::nullopt;
    }
    return parsed;
  }

  void append_json_escaped(std::string &out, const std::string &value)
  {
    out.push_back('"');
    for (const char ch : value)
    {
      const auto byte = static_cast<unsigned char>(ch);
      switch (ch)
      {
        case '"':
          out += "\\\"";
          break;
        case '\\':
          out += "\\\\";
          break;
        case '\n':
          out += "\\n";
          break;
        case '\r':
          out += "\\r";
          break;
        case '\t':
          out += "\\t";
          break;
        default:
          if (byte < 0x20)
          {
            out += "\\u00";
            constexpr char hex[] = "0123456789abcdef";
            out.push_back(hex[(byte >> 4) & 0x0f]);
            out.push_back(hex[byte & 0x0f]);
          }
          else
          {
            out.push_back(ch);
          }
      }
    }
    out.push_back('"');
  }

  void append_otlp_path_attribute(std::string &out, bool &first, const std::string &target)
  {
    if (target.empty())
    {
      return;
    }
    if (!first)
    {
      out.push_back(',');
    }
    first = false;
    out += "{\"key\":\"url.path\",\"value\":{\"stringValue\":";
    const std::size_t query = target.find('?');
    if (query == std::string::npos)
    {
      append_json_escaped(out, target);
    }
    else
    {
      append_json_escaped(out, target.substr(0, query));
    }
    out += "}}";
  }

  std::pair<std::string, std::string> protocol_pair_for_otlp(const std::string &protocol)
  {
    constexpr const char *prefix = "HTTP/";
    if (protocol.rfind(prefix, 0) == 0 && protocol.size() > std::char_traits<char>::length(prefix))
    {
      return {"http", protocol.substr(std::char_traits<char>::length(prefix))};
    }
    return {"", ""};
  }

  void append_otlp_string_attribute(std::string &out, bool &first, const std::string &key, const std::string &value)
  {
    if (value.empty())
    {
      return;
    }
    if (!first)
    {
      out.push_back(',');
    }
    first = false;
    out += "{\"key\":";
    append_json_escaped(out, key);
    out += ",\"value\":{\"stringValue\":";
    append_json_escaped(out, value);
    out += "}}";
  }

  void append_otlp_int_attribute(std::string &out, bool &first, const std::string &key, int value)
  {
    if (value < 0)
    {
      return;
    }
    if (!first)
    {
      out.push_back(',');
    }
    first = false;
    out += "{\"key\":";
    append_json_escaped(out, key);
    out += ",\"value\":{\"intValue\":\"";
    out += std::to_string(value);
    out += "\"}}";
  }

  void append_otlp_bool_attribute(std::string &out, bool &first, const std::string &key, bool value)
  {
    if (!first)
    {
      out.push_back(',');
    }
    first = false;
    out += "{\"key\":";
    append_json_escaped(out, key);
    out += ",\"value\":{\"boolValue\":";
    out += value ? "true" : "false";
    out += "}}";
  }

  void append_fixed_hex64(std::string &out, std::uint64_t value)
  {
    constexpr char hex[] = "0123456789abcdef";
    for (int shift = 60; shift >= 0; shift -= 4)
    {
      out.push_back(hex[(value >> shift) & 0x0f]);
    }
  }

  void append_native_otlp_trace_id(std::string &out, const Vajra::runtime::RequestSpanEvent &event)
  {
    if (!event.trace_id.empty() && !event.span_id.empty())
    {
      append_json_escaped(out, event.trace_id);
      return;
    }
    const std::uint64_t high = sampled_hash_value(static_cast<std::uint64_t>(event.duration_nanoseconds));
    const std::uint64_t low = sampled_hash_value(native_otlp_id_counter.fetch_add(1, std::memory_order_acq_rel));
    out.push_back('"');
    append_fixed_hex64(out, high);
    append_fixed_hex64(out, low);
    out.push_back('"');
  }

  void append_native_otlp_span_id(std::string &out, const Vajra::runtime::RequestSpanEvent &event)
  {
    const std::uint64_t value = sampled_hash_value(static_cast<std::uint64_t>(event.duration_nanoseconds) ^
                                                   native_otlp_id_counter.fetch_add(1, std::memory_order_acq_rel));
    out.push_back('"');
    append_fixed_hex64(out, value);
    out.push_back('"');
  }

  void append_native_otlp_span(std::string &out, const Vajra::runtime::RequestSpanEvent &event)
  {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto end_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    const auto start_ns = end_ns - event.duration_nanoseconds;
    out += "{\"traceId\":";
    append_native_otlp_trace_id(out, event);
    out += ",\"spanId\":";
    append_native_otlp_span_id(out, event);
    if (!event.span_id.empty())
    {
      out += ",\"parentSpanId\":";
      append_json_escaped(out, event.span_id);
    }
    out += ",\"name\":";
    append_json_escaped(out, event.lifecycle_span ? "vajra." + event.event_name : event.method.empty() ? std::string("HTTP request") : event.method);
    out += ",\"kind\":";
    out += event.lifecycle_span ? "1" : "2";
    out += ",\"startTimeUnixNano\":";
    append_json_escaped(out, std::to_string(start_ns));
    out += ",\"endTimeUnixNano\":";
    append_json_escaped(out, std::to_string(end_ns));
    out += ",\"attributes\":[";
    bool first = true;
    if (event.lifecycle_span)
    {
      append_otlp_string_attribute(out, first, "vajra.worker.lifecycle_event", event.event_name);
      append_otlp_string_attribute(out, first, "vajra.worker.lifecycle_state", event.lifecycle_state);
      append_otlp_string_attribute(out, first, "vajra.worker.health_state", event.health_state);
      append_otlp_string_attribute(out, first, "vajra.worker.recovery_state", event.recovery_state);
      append_otlp_bool_attribute(out, first, "vajra.worker.available", event.available);
      append_otlp_string_attribute(out, first, "vajra.worker.exit_classification", event.exit_classification);
      append_otlp_bool_attribute(out, first, "vajra.worker.terminal_replacement_failure", event.terminal_replacement_failure);
      append_otlp_bool_attribute(out, first, "vajra.worker.replacement_needed", event.replacement_needed);
      append_otlp_int_attribute(out, first, "vajra.worker.exit_detail", event.exit_detail);
    }
    else
    {
      const auto [protocol_name, protocol_version] = protocol_pair_for_otlp(event.protocol);
      append_otlp_string_attribute(out, first, "http.request.method", event.method);
      append_otlp_path_attribute(out, first, event.target);
      append_otlp_int_attribute(out, first, "http.response.status_code", event.status_code);
      append_otlp_string_attribute(out, first, "server.address", event.host);
      append_otlp_string_attribute(out, first, "network.protocol.name", protocol_name);
      append_otlp_string_attribute(out, first, "network.protocol.version", protocol_version);
      append_otlp_string_attribute(out, first, "vajra.request.outcome", event.outcome);
      append_otlp_string_attribute(out, first, "vajra.failure.kind", event.failure_kind);
      append_otlp_bool_attribute(out, first, "vajra.response.sent", event.response_sent);
      append_otlp_string_attribute(out, first, "vajra.connection.outcome", event.connection_outcome);
    }
    append_otlp_int_attribute(out, first, "vajra.worker.index", event.worker_index);
    append_otlp_int_attribute(out, first, "vajra.worker.pid", static_cast<int>(event.worker_pid));
    out += "],\"status\":{\"code\":";
    if (event.lifecycle_span || native_request_span_success(event))
    {
      out += "1";
    }
    else
    {
      out += "2,\"message\":";
      append_json_escaped(out, event.error_message);
    }
    out += "}}";
  }

  Vajra::runtime::RequestSpanEvent request_span_event_from_access(
      const Vajra::runtime::AccessLogEvent &event,
      const std::string &outcome,
      const std::string &failure_kind,
      bool response_sent,
      const std::string &error_message)
  {
    Vajra::runtime::RequestSpanEvent span;
    span.method = event.method;
    span.target = event.target;
    span.status_code = event.status_code;
    span.duration_nanoseconds = event.duration_nanoseconds;
    span.protocol = event.protocol;
    span.host = event.host;
    span.outcome = outcome;
    span.failure_kind = failure_kind;
    span.response_sent = response_sent;
    span.connection_outcome = event.connection_outcome;
    span.worker_index = event.worker_index;
    span.worker_pid = event.worker_pid;
    span.trace_id = event.trace_id;
    span.span_id = event.span_id;
    span.error_message = error_message;
    return span;
  }

  std::string native_otlp_payload(
      const std::vector<Vajra::runtime::RequestSpanEvent> &events,
      const std::string &service_name)
  {
    std::string body;
    body.reserve(512 + (events.size() * 512));
    body += "{\"resourceSpans\":[{\"resource\":{\"attributes\":[{\"key\":\"service.name\",\"value\":{\"stringValue\":";
    append_json_escaped(body, service_name);
    body += "}}]},\"scopeSpans\":[{\"scope\":{\"name\":";
    append_json_escaped(body, service_name);
    body += "},\"spans\":[";
    for (std::size_t index = 0; index < events.size(); ++index)
    {
      if (index != 0)
      {
        body.push_back(',');
      }
      append_native_otlp_span(body, events[index]);
    }
    body += "]}]}]}";
    return body;
  }

  bool native_otlp_post(const NativeOtlpEndpoint &endpoint, const std::string &body)
  {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *result = nullptr;
    if (getaddrinfo(endpoint.host.c_str(), endpoint.port.c_str(), &hints, &result) != 0)
    {
      return false;
    }
    const std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> addresses(result, freeaddrinfo);
    int fd = -1;
    for (addrinfo *cursor = addresses.get(); cursor != nullptr; cursor = cursor->ai_next)
    {
      fd = socket(cursor->ai_family, cursor->ai_socktype, cursor->ai_protocol);
      if (fd < 0)
      {
        continue;
      }
      if (connect(fd, cursor->ai_addr, cursor->ai_addrlen) == 0)
      {
        break;
      }
      close(fd);
      fd = -1;
    }
    if (fd < 0)
    {
      return false;
    }
    prepare_socket_write(fd);
    std::ostringstream request;
    request << "POST " << endpoint.path << " HTTP/1.1\r\n"
            << "Host: " << endpoint.host_header << "\r\n"
            << "Content-Type: application/json\r\n"
            << "Content-Length: " << body.size() << "\r\n"
            << "Connection: close\r\n\r\n";
    const std::string head = request.str();
    const bool ok = send_all(fd, head.data(), head.size()) && send_all(fd, body.data(), body.size());
    close(fd);
    return ok;
  }

  std::vector<Vajra::runtime::RequestSpanEvent> drain_native_otlp_batch(std::size_t limit)
  {
    std::vector<Vajra::runtime::RequestSpanEvent> drained;
    const std::lock_guard<std::mutex> lock(request_observability_mutex);
    const std::size_t count = std::min(limit, native_otlp_span_events.size());
    drained.reserve(count);
    for (std::size_t index = 0; index < count; ++index)
    {
      drained.push_back(std::move(native_otlp_span_events.front()));
      native_otlp_span_events.pop_front();
    }
    return drained;
  }

  void native_otlp_export_loop()
  {
    while (true)
    {
      NativeOtlpEndpoint endpoint;
      std::string service_name;
      {
        std::unique_lock<std::mutex> lock(request_observability_mutex);
        request_span_condition.wait_for(lock, kNativeOtlpFlushInterval, [] {
          return native_otlp_span_events.size() >= kNativeOtlpBatchSize ||
                 native_otlp_exporter.stopping.load(std::memory_order_acquire);
        });
        if (native_otlp_span_events.empty() && native_otlp_exporter.stopping.load(std::memory_order_acquire))
        {
          break;
        }
      }
      {
        std::lock_guard<std::mutex> lock(native_otlp_exporter.mutex);
        endpoint = native_otlp_exporter.endpoint;
        service_name = native_otlp_exporter.service_name;
      }
      auto events = drain_native_otlp_batch(kNativeOtlpBatchSize);
      if (!events.empty())
      {
        (void)native_otlp_post(endpoint, native_otlp_payload(events, service_name));
      }
    }
  }

  void start_native_otlp_exporter(const std::string &endpoint, const std::string &service_name)
  {
    const auto parsed = parse_native_otlp_endpoint(endpoint);
    if (!parsed)
    {
      native_otlp_export_enabled.store(false, std::memory_order_release);
      return;
    }
    std::lock_guard<std::mutex> lock(native_otlp_exporter.mutex);
    if (native_otlp_exporter.running.load(std::memory_order_acquire) &&
        native_otlp_exporter.owner_pid.load(std::memory_order_acquire) == getpid())
    {
      return;
    }
    if (native_otlp_exporter.running.load(std::memory_order_acquire) &&
        native_otlp_exporter.owner_pid.load(std::memory_order_acquire) != getpid())
    {
      if (native_otlp_exporter.worker.joinable())
      {
        native_otlp_exporter.worker.detach();
      }
      native_otlp_export_enabled.store(false, std::memory_order_release);
      native_otlp_exporter.stopping.store(false, std::memory_order_release);
      native_otlp_exporter.running.store(false, std::memory_order_release);
      native_otlp_exporter.owner_pid.store(-1, std::memory_order_release);
    }
    native_otlp_exporter.endpoint = *parsed;
    native_otlp_exporter.service_name = service_name.empty() ? "vajra" : service_name;
    native_otlp_exporter.stopping.store(false, std::memory_order_release);
    native_otlp_exporter.owner_pid.store(getpid(), std::memory_order_release);
    native_otlp_exporter.running.store(true, std::memory_order_release);
    native_otlp_export_enabled.store(true, std::memory_order_release);
    native_otlp_exporter.worker = std::thread(native_otlp_export_loop);
  }

  void stop_native_otlp_exporter()
  {
    std::thread worker;
    {
      std::lock_guard<std::mutex> lock(native_otlp_exporter.mutex);
      native_otlp_export_enabled.store(false, std::memory_order_release);
      native_otlp_exporter.stopping.store(true, std::memory_order_release);
      if (native_otlp_exporter.owner_pid.load(std::memory_order_acquire) == getpid() &&
          native_otlp_exporter.worker.joinable())
      {
        worker = std::move(native_otlp_exporter.worker);
      }
      native_otlp_exporter.running.store(false, std::memory_order_release);
      native_otlp_exporter.owner_pid.store(-1, std::memory_order_release);
    }
    request_span_condition.notify_all();
    if (worker.joinable())
    {
      worker.join();
    }
  }

  void enqueue_runtime_request_observability_event(
      const Vajra::runtime::AccessLogEvent &event,
      const std::string &outcome,
      const std::string &failure_kind,
      bool response_sent,
      const std::string &error_message)
  {
    native_request_observability_events_total.fetch_add(1, std::memory_order_acq_rel);
    if (!native_request_observability_success(event, failure_kind, response_sent))
    {
      native_request_observability_errors_total.fetch_add(1, std::memory_order_acq_rel);
    }
    if (native_request_admission_outcome(outcome))
    {
      native_request_admission_rejections_total.fetch_add(1, std::memory_order_acq_rel);
    }

    if (!request_observability_enabled.load(std::memory_order_acquire) &&
        !native_otlp_export_enabled.load(std::memory_order_acquire))
    {
      return;
    }

    const auto span = request_span_event_from_access(event, outcome, failure_kind, response_sent, error_message);
    const std::lock_guard<std::mutex> lock(request_observability_mutex);
    if (request_observability_enabled.load(std::memory_order_acquire))
    {
      Vajra::runtime::RequestObservabilityEvent queued;
      queued.access = event;
      queued.outcome = outcome;
      queued.failure_kind = failure_kind;
      queued.response_sent = response_sent;
      queued.error_message = error_message;
      queued.timestamp = Vajra::runtime::utc_timestamp();
      request_observability_events.push_back(std::move(queued));
    }
    if (native_otlp_export_enabled.load(std::memory_order_acquire))
    {
      native_otlp_span_events.push_back(span);
      if (native_otlp_span_events.size() >= kNativeOtlpBatchSize)
      {
        request_span_condition.notify_one();
      }
    }
  }

  void enqueue_runtime_request_span_event(Vajra::runtime::RequestSpanEvent event)
  {
    native_request_observability_events_total.fetch_add(1, std::memory_order_acq_rel);
    if (!native_request_span_success(event))
    {
      native_request_observability_errors_total.fetch_add(1, std::memory_order_acq_rel);
    }
    if (native_request_admission_outcome(event.outcome))
    {
      native_request_admission_rejections_total.fetch_add(1, std::memory_order_acq_rel);
    }

    if (!request_observability_enabled.load(std::memory_order_acquire) &&
        !native_otlp_export_enabled.load(std::memory_order_acquire))
    {
      return;
    }

    const std::lock_guard<std::mutex> lock(request_observability_mutex);
    if (native_otlp_export_enabled.load(std::memory_order_acquire))
    {
      native_otlp_span_events.push_back(std::move(event));
      if (native_otlp_span_events.size() >= kNativeOtlpBatchSize)
      {
        request_span_condition.notify_one();
      }
    }
  }

  void enqueue_runtime_lifecycle_span_event(
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
    if (!native_otlp_export_enabled.load(std::memory_order_acquire) || !Vajra::runtime::runtime_trace_sampled(""))
    {
      return;
    }

    Vajra::runtime::RequestSpanEvent span;
    span.lifecycle_span = true;
    span.event_name = event_name;
    span.duration_nanoseconds = 0;
    span.worker_index = static_cast<int>(worker_index);
    span.worker_pid = pid;
    span.lifecycle_state = worker_lifecycle_state_name(lifecycle_state);
    span.health_state = worker_health_state_name(health_state);
    span.recovery_state = worker_recovery_state_name(recovery_state);
    span.available = available;
    span.exit_classification = worker_exit_classification_name(exit_classification);
    span.terminal_replacement_failure = terminal_replacement_failure;
    span.replacement_needed = replacement_needed;
    span.exit_detail = exit_detail;

    const std::lock_guard<std::mutex> lock(request_observability_mutex);
    native_otlp_span_events.push_back(std::move(span));
    if (native_otlp_span_events.size() >= kNativeOtlpBatchSize)
    {
      request_span_condition.notify_one();
    }
  }

  void write_runtime_line(const std::string &line)
  {
    service_reopen_signal_if_requested();
    if (enqueue_log_event(LogEvent{LogEventKind::runtime, false, line, {}}))
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
    service_reopen_signal_if_requested();
    if (enqueue_log_event(LogEvent{LogEventKind::error, false, line, {}}))
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
    service_reopen_signal_if_requested();
    if (enqueue_log_event(LogEvent{LogEventKind::access, structured_logging_enabled(), line, {}}))
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
    const std::string &error_log,
    const std::string &access_log_format)
{
  std::string warning_message;
  stop_runtime_logging_worker();
  install_reopen_signal_handler();
  std::lock_guard<std::mutex> lock(logging_mutex);
  close_configured_fd(logging_config.access_log_fd, logging_config.close_access_log_fd, STDOUT_FILENO);
  close_configured_fd(logging_config.error_log_fd, logging_config.close_error_log_fd, STDERR_FILENO);
  logging_config.structured_logs = structured_logs;
  logging_config.access_log_path = access_log;
  logging_config.error_log_path = error_log;
  logging_config.access_log_format = access_log_format;
  logging_config.access_field_needs = field_needs_for_format(access_log_format, structured_logs);
  logging_config.access_log_disabled = access_log.empty() || access_log == "/dev/null";
  access_log_disabled.store(logging_config.access_log_disabled, std::memory_order_release);
  structured_logs_enabled.store(structured_logs, std::memory_order_release);
  publish_access_field_needs(logging_config.access_field_needs);

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
    const std::string &trace_service_name,
    bool active_context_required)
{
  const std::lock_guard<std::mutex> lock(logging_mutex);
  logging_config.trace_enabled = trace_enabled;
  logging_config.trace_endpoint = trace_endpoint;
  logging_config.trace_service_name = trace_service_name;
  logging_config.trace_active_context_required = active_context_required;
  trace_active_context_required_snapshot.store(active_context_required, std::memory_order_release);
  if (!trace_enabled)
  {
    logging_config.trace_available = false;
    trace_available_snapshot.store(false, std::memory_order_release);
    trace_active_context_required_snapshot.store(false, std::memory_order_release);
    trace_sample_threshold.store(0, std::memory_order_release);
    stop_native_otlp_exporter();
    return;
  }
  stop_native_otlp_exporter();
}

void Vajra::runtime::start_runtime_tracing_worker()
{
  LoggingConfig config_snapshot;
  {
    const std::lock_guard<std::mutex> lock(logging_mutex);
    config_snapshot.trace_enabled = logging_config.trace_enabled;
    config_snapshot.trace_endpoint = logging_config.trace_endpoint;
    config_snapshot.trace_service_name = logging_config.trace_service_name;
    config_snapshot.trace_active_context_required = logging_config.trace_active_context_required;
  }

  if (config_snapshot.trace_enabled &&
      !config_snapshot.trace_active_context_required &&
      !config_snapshot.trace_endpoint.empty())
  {
    start_native_otlp_exporter(config_snapshot.trace_endpoint, config_snapshot.trace_service_name);
    return;
  }

  stop_native_otlp_exporter();
}

void Vajra::runtime::stop_runtime_tracing_worker()
{
  stop_native_otlp_exporter();
}

void Vajra::runtime::set_runtime_trace_sample_ratio(double sample_ratio)
{
  const std::lock_guard<std::mutex> lock(logging_mutex);
  logging_config.trace_sample_ratio = sample_ratio;
  trace_sample_threshold.store(trace_sample_threshold_for(sample_ratio), std::memory_order_release);
}

void Vajra::runtime::start_runtime_logging_worker()
{
  LoggingConfig config_snapshot;
  {
    const std::lock_guard<std::mutex> lock(logging_mutex);
    config_snapshot.structured_logs = logging_config.structured_logs;
    config_snapshot.access_log_disabled = logging_config.access_log_disabled;
    config_snapshot.access_log_format = logging_config.access_log_format;
    config_snapshot.access_field_needs = logging_config.access_field_needs;
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

  LogNode *stub = acquire_log_node();
  if (stub == nullptr)
  {
    async_logger.running.store(false, std::memory_order_release);
    async_logger.owner_pid.store(-1, std::memory_order_release);
    return;
  }
  async_logger.head = stub;
  async_logger.tail.store(stub, std::memory_order_release);
  async_logger.pending.store(0, std::memory_order_release);
  async_logger.structured_logs = config_snapshot.structured_logs;
  async_logger.access_log_disabled = config_snapshot.access_log_disabled;
  async_logger.access_log_format = config_snapshot.access_log_format;
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

#ifdef VAJRA_RUNTIME_TESTING
std::size_t Vajra::runtime::runtime_logging_node_block_count()
{
  std::lock_guard<std::mutex> lock(async_logger.pool_mutex);
  return async_logger.blocks.size();
}
#endif

void Vajra::runtime::set_runtime_tracing_available(bool available)
{
  const std::lock_guard<std::mutex> lock(logging_mutex);
  logging_config.trace_available = available;
  trace_available_snapshot.store(available, std::memory_order_release);
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

bool Vajra::runtime::runtime_tracing_active_context_required()
{
  return trace_available_snapshot.load(std::memory_order_acquire) &&
         trace_active_context_required_snapshot.load(std::memory_order_acquire);
}

bool Vajra::runtime::runtime_trace_sampled(const std::string &traceparent)
{
  if (!request_observability_enabled.load(std::memory_order_acquire) &&
      !native_otlp_export_enabled.load(std::memory_order_acquire))
  {
    return false;
  }
  const int parent_sampled = traceparent_sample_flag(traceparent);
  if (parent_sampled >= 0)
  {
    return parent_sampled == 1;
  }

  const std::uint64_t threshold = trace_sample_threshold.load(std::memory_order_acquire);
  if (threshold == 0)
  {
    return false;
  }
  if (threshold == std::numeric_limits<std::uint64_t>::max())
  {
    return true;
  }

  const std::uint64_t sample_value = sampled_hash_value(trace_sample_counter.fetch_add(1, std::memory_order_acq_rel));
  return sample_value <= threshold;
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

Vajra::runtime::AccessLogFieldNeeds Vajra::runtime::access_log_field_needs()
{
  return Vajra::runtime::AccessLogFieldNeeds{
      access_need_host.load(std::memory_order_acquire),
      access_need_user_agent.load(std::memory_order_acquire),
      access_need_referer.load(std::memory_order_acquire),
      access_need_request_id.load(std::memory_order_acquire),
      access_need_trace_context.load(std::memory_order_acquire) || trace_available_snapshot.load(std::memory_order_acquire)};
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

void Vajra::runtime::set_runtime_request_observability_callback(void *callback)
{
#ifdef VAJRA_RUNTIME_HAS_RUBY
  static bool callback_rooted = false;
  if (!callback_rooted)
  {
    rb_global_variable(&runtime_request_observability_callback);
    callback_rooted = true;
  }
  const std::lock_guard<std::mutex> lock(logging_mutex);
  runtime_request_observability_callback = reinterpret_cast<VALUE>(callback);
  request_observability_enabled.store(!NIL_P(runtime_request_observability_callback), std::memory_order_release);
#else
  request_observability_enabled.store(callback != nullptr, std::memory_order_release);
#endif
}

bool Vajra::runtime::runtime_request_observability_enabled()
{
  return request_observability_enabled.load(std::memory_order_acquire);
}

bool Vajra::runtime::runtime_request_span_observability_enabled()
{
  return request_observability_enabled.load(std::memory_order_acquire) ||
         native_otlp_export_enabled.load(std::memory_order_acquire);
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
    enqueue_runtime_lifecycle_span_event(
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
  enqueue_runtime_lifecycle_span_event(
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

void Vajra::runtime::log_access_event(const AccessLogEvent &event)
{
  service_reopen_signal_if_requested();
  if (access_log_disabled.load(std::memory_order_acquire))
  {
    return;
  }

  if (enqueue_log_event(LogEvent{
          LogEventKind::access,
          structured_logs_enabled.load(std::memory_order_acquire),
          "",
          event}))
  {
    return;
  }

  LogEvent log_event{LogEventKind::access, structured_logging_enabled(), "", event};
  write_access_line(formatted_access_line(log_event));
  flush_runtime_streams();
}

std::vector<Vajra::runtime::RequestObservabilityEvent> Vajra::runtime::drain_runtime_request_observability_events(std::size_t limit)
{
  std::vector<Vajra::runtime::RequestObservabilityEvent> drained;
  if (limit == 0)
  {
    return drained;
  }

  const std::lock_guard<std::mutex> lock(request_observability_mutex);
  const std::size_t count = std::min(limit, request_observability_events.size());
  drained.reserve(count);
  for (std::size_t index = 0; index < count; ++index)
  {
    drained.push_back(std::move(request_observability_events.front()));
    request_observability_events.pop_front();
  }
  return drained;
}

std::uint64_t Vajra::runtime::runtime_native_request_observability_events_total()
{
  return native_request_observability_events_total.load(std::memory_order_acquire);
}

std::uint64_t Vajra::runtime::runtime_native_request_observability_errors_total()
{
  return native_request_observability_errors_total.load(std::memory_order_acquire);
}

std::uint64_t Vajra::runtime::runtime_native_request_admission_rejections_total()
{
  return native_request_admission_rejections_total.load(std::memory_order_acquire);
}

void Vajra::runtime::emit_runtime_request_observability_event(
    const AccessLogEvent &event,
    const std::string &outcome,
    const std::string &failure_kind,
    bool response_sent,
    const std::string &error_message)
{
  enqueue_runtime_request_observability_event(event, outcome, failure_kind, response_sent, error_message);
}

void Vajra::runtime::emit_runtime_request_span_event(const RequestSpanEvent &event)
{
  enqueue_runtime_request_span_event(event);
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
