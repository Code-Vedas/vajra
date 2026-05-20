// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "vajra.hpp"
#include "listener/listener_socket.hpp"
#include "rack/rack_request_executor.hpp"
#include "ruby.h"
#include "ruby/thread.h"

#include <csignal>
#include <signal.h>
#include <algorithm>
#include <cstdlib>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <exception>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

namespace
{
  volatile std::sig_atomic_t shutting_down = 0;
  bool runtime_shutdown_started = false;
  std::mutex server_mutex;
  std::shared_ptr<Vajra::Server> server_instance;
  std::vector<pid_t> worker_pids;
  std::vector<int> worker_request_channel_fds;
  bool stop_requested = false;
  bool worker_startup_in_progress = false;
  ID id_port;
  ID id_host;
  ID id_workers;
  ID id_threads;
  ID id_max_request_head_bytes;
  ID id_max_connections;
  ID id_queue_capacity;
  ID id_scheduler_policy;
  ID id_request_timeout;
  ID id_request_head_timeout;
  ID id_first_data_timeout;
  ID id_persistent_timeout;
  ID id_worker_timeout;
  ID id_log_level;
  ID id_runtime_role;
  std::mutex boot_callback_mutex;
  VALUE boot_callback = Qnil;
  constexpr const char *kMasterPreloadRuntimeRole = "ruby_master_preload";
  constexpr const char *kNativeRuntimeControlRole = "native_runtime_control";
  constexpr const char *kWorkerBootstrapRuntimeRole = "ruby_worker_bootstrap";
  constexpr const char *kMasterWorkerMode = "master_worker";
  constexpr std::size_t kMaxWorkerBootstrapStringPayloadBytes = 64 * 1024;

  struct RuntimeConfig
  {
    std::string host;
    int port;
    int workers;
    std::size_t min_threads;
    std::size_t max_threads;
    std::size_t max_connections;
    std::size_t queue_capacity;
    std::string scheduler_policy;
    std::size_t max_request_head_bytes;
    std::size_t request_timeout_seconds;
    int request_head_timeout_seconds;
    int first_data_timeout_seconds;
    int persistent_timeout_seconds;
    int worker_timeout_seconds;
    std::string log_level;
  };

  struct BootContractConfig
  {
    int port;
    std::size_t max_request_head_bytes;
    std::string runtime_role;
  };

  struct BootDiagnostic
  {
    std::string code;
    std::string category;
    std::string message;
  };

  enum class BootStatus
  {
    pending,
    ready,
    failed,
  };

  struct BootContractResult
  {
    BootStatus status;
    std::string runtime_role;
    std::optional<BootDiagnostic> diagnostic;
  };

  enum class WorkerBootstrapStatus : std::uint8_t
  {
    ready = 1,
    failed = 2,
  };

  struct WorkerBootstrapReport
  {
    WorkerBootstrapStatus status;
    std::optional<BootDiagnostic> diagnostic;
  };

  struct ProtectedIntegerConversion
  {
    VALUE value;
    VALUE result;
  };

  struct ProtectedStringConversion
  {
    VALUE value;
    VALUE result;
  };

  struct ServerRunContext
  {
    Vajra::Server *server;
    std::string error_message;
  };

  struct WorkerWaitContext
  {
    std::vector<pid_t> pids;
    std::string error_message;
  };

  struct OptionValidationContext
  {
    bool valid;
    std::string invalid_option_name;
  };

  void handle_signal(int sig)
  {
    if (sig == SIGINT || sig == SIGTERM)
    {
      shutting_down = 1;
    }
  }

  std::string utc_timestamp()
  {
    const auto now = std::chrono::system_clock::now();
    const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm utc_time{};
    gmtime_r(&now_time, &utc_time);

    std::ostringstream timestamp;
    timestamp << std::put_time(&utc_time, "%Y-%m-%dT%H:%M:%SZ");
    return timestamp.str();
  }

  std::string runtime_environment_name()
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

  void log_runtime_banner_start(
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

  void log_worker_booted(int worker_index, pid_t pid, double boot_seconds)
  {
    std::ostringstream message;
    message << "[" << getppid() << "] - Worker " << worker_index
            << " (PID: " << pid << ") booted in "
            << std::fixed << std::setprecision(2) << boot_seconds << "s";
    std::cout << message.str() << std::endl;
  }

  void log_runtime_shutdown_begin()
  {
    std::cout << "[" << getpid() << "] - Gracefully shutting down workers..." << std::endl;
  }

  void log_runtime_shutdown_complete()
  {
    std::cout << "[" << getpid() << "] === vajra shutdown: " << utc_timestamp() << " ===" << std::endl;
    std::cout << "[" << getpid() << "] - Goodbye!" << std::endl;
  }

  void begin_runtime_shutdown()
  {
    std::lock_guard<std::mutex> lock(server_mutex);
    if (runtime_shutdown_started)
    {
      return;
    }

    runtime_shutdown_started = true;
    log_runtime_shutdown_begin();
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

  [[noreturn]] void raise_ruby_runtime_error(const char *prefix, const std::exception &error)
  {
    rb_raise(rb_eRuntimeError, "%s: %s", prefix, error.what());
  }

  VALUE protected_rb_integer(VALUE data)
  {
    auto *conversion = reinterpret_cast<ProtectedIntegerConversion *>(data);
    conversion->result = rb_Integer(conversion->value);
    return conversion->result;
  }

  VALUE protected_rb_obj_as_string(VALUE data)
  {
    auto *conversion = reinterpret_cast<ProtectedStringConversion *>(data);
    conversion->result = rb_obj_as_string(conversion->value);
    return conversion->result;
  }

  VALUE protected_rb_num2long(VALUE data)
  {
    auto *value = reinterpret_cast<VALUE *>(data);
    return LONG2NUM(NUM2LONG(*value));
  }

  VALUE protected_format_ruby_exception_message(VALUE)
  {
    VALUE exception = rb_errinfo();
    if (NIL_P(exception))
    {
      return rb_str_new_cstr("Ruby exception");
    }

    return rb_obj_as_string(exception);
  }

  VALUE protected_rb_hash_lookup(VALUE data)
  {
    auto *lookup = reinterpret_cast<VALUE *>(data);
    const VALUE hash = lookup[0];
    const VALUE key = lookup[1];
    return rb_hash_lookup(hash, key);
  }

  std::string ruby_string_value(VALUE value, const char *error_message)
  {
    if (RB_TYPE_P(value, T_STRING) == 0)
    {
      throw std::runtime_error(error_message);
    }

    return std::string(RSTRING_PTR(value), static_cast<std::size_t>(RSTRING_LEN(value)));
  }

  VALUE ruby_boot_request_from(const BootContractConfig &config)
  {
    VALUE boot_request = rb_hash_new();
    rb_hash_aset(boot_request, ID2SYM(id_port), INT2NUM(config.port));
    rb_hash_aset(boot_request, ID2SYM(id_max_request_head_bytes), SIZET2NUM(config.max_request_head_bytes));
    rb_hash_aset(
        boot_request,
        ID2SYM(id_runtime_role),
        rb_str_new(config.runtime_role.data(), static_cast<long>(config.runtime_role.size())));
    return boot_request;
  }

  VALUE protected_execute_boot_callback(VALUE data)
  {
    auto *config = reinterpret_cast<BootContractConfig *>(data);
    VALUE callback = Qnil;
    {
      const std::lock_guard<std::mutex> callback_lock(boot_callback_mutex);
      callback = boot_callback;
    }

    if (NIL_P(callback))
    {
      rb_raise(
          rb_eRuntimeError,
          "Ruby boot contract callback is not installed. Require \"vajra\" or call Vajra::Internal::Boot.install! before starting Vajra.");
    }

    VALUE boot_request = ruby_boot_request_from(*config);
    VALUE arguments[] = {boot_request};
    return rb_proc_call(callback, rb_ary_new_from_values(1, arguments));
  }

  BootStatus boot_status_from_ruby(VALUE status)
  {
    const std::string status_value = ruby_string_value(status, "Ruby boot contract returned a non-string status");
    if (status_value == "pending")
    {
      return BootStatus::pending;
    }
    if (status_value == "ready")
    {
      return BootStatus::ready;
    }
    if (status_value == "failed")
    {
      return BootStatus::failed;
    }

    throw std::runtime_error("Ruby boot contract returned an unsupported status: " + status_value);
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

  std::optional<BootDiagnostic> boot_diagnostic_from_ruby(VALUE diagnostic)
  {
    if (NIL_P(diagnostic))
    {
      return std::nullopt;
    }

    if (TYPE(diagnostic) != T_ARRAY || RARRAY_LEN(diagnostic) != 3)
    {
      throw std::runtime_error("Ruby boot contract returned an invalid diagnostic");
    }

    return BootDiagnostic{
        ruby_string_value(rb_ary_entry(diagnostic, 0), "Ruby boot contract returned a non-string diagnostic code"),
        ruby_string_value(rb_ary_entry(diagnostic, 1), "Ruby boot contract returned a non-string diagnostic category"),
        ruby_string_value(rb_ary_entry(diagnostic, 2), "Ruby boot contract returned a non-string diagnostic message")};
  }

  BootContractResult boot_result_from_ruby(VALUE result)
  {
    if (TYPE(result) != T_ARRAY || RARRAY_LEN(result) != 3)
    {
      throw std::runtime_error("Ruby boot contract returned an invalid result");
    }

    return BootContractResult{
        boot_status_from_ruby(rb_ary_entry(result, 0)),
        ruby_string_value(rb_ary_entry(result, 1), "Ruby boot contract returned a non-string runtime role"),
        boot_diagnostic_from_ruby(rb_ary_entry(result, 2))};
  }

  BootContractResult run_boot_contract(const BootContractConfig &config)
  {
    int state = 0;
    VALUE result = rb_protect(
        protected_execute_boot_callback,
        reinterpret_cast<VALUE>(const_cast<BootContractConfig *>(&config)),
        &state);
    if (state == 0)
    {
      return boot_result_from_ruby(result);
    }

    VALUE message = Qnil;
    int message_state = 0;
    message = rb_protect(protected_format_ruby_exception_message, Qnil, &message_state);
    rb_set_errinfo(Qnil);

    if (message_state != 0 || NIL_P(message))
    {
      throw std::runtime_error("Ruby boot contract execution failed: Ruby exception");
    }

    throw std::runtime_error("Ruby boot contract execution failed: " + std::string(StringValueCStr(message)));
  }

  void ensure_ready_boot_result(const BootContractResult &result)
  {
    switch (result.status)
    {
      case BootStatus::ready:
        return;
      case BootStatus::pending:
        throw std::runtime_error("Ruby boot contract did not reach ready state");
      case BootStatus::failed:
        if (!result.diagnostic.has_value())
        {
          throw std::runtime_error("Ruby boot failed without diagnostic details");
        }

        throw std::runtime_error(
            "Ruby boot failed (" + result.diagnostic->code + "/" + result.diagnostic->category + "): " +
            result.diagnostic->message);
    }

    throw std::runtime_error("Ruby boot contract returned an unknown state");
  }

  BootDiagnostic diagnostic_for_boot_result_failure(const BootContractResult &result)
  {
    switch (result.status)
    {
      case BootStatus::failed:
        if (result.diagnostic.has_value())
        {
          return *result.diagnostic;
        }
        return BootDiagnostic{
            "missing_boot_diagnostic",
            "contract",
            "Ruby boot failed without diagnostic details"};
      case BootStatus::pending:
        return BootDiagnostic{
            "boot_not_ready",
            "contract",
            "Ruby boot contract did not reach ready state"};
      case BootStatus::ready:
        return BootDiagnostic{
            "unexpected_ready_state",
            "contract",
            "Ruby worker bootstrap reported ready when a failure diagnostic was requested"};
    }

    return BootDiagnostic{
        "unknown_boot_state",
        "contract",
        "Ruby boot contract returned an unknown state"};
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

  void report_worker_boot_failed(int write_fd, const BootDiagnostic &diagnostic)
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
          BootDiagnostic{
              read_string_payload(read_fd),
              read_string_payload(read_fd),
              read_string_payload(read_fd)}};
    }

    throw std::runtime_error("worker reported an unknown bootstrap state");
  }

  [[noreturn]] void exit_worker_bootstrap_failure(int write_fd, const BootDiagnostic &diagnostic, int exit_code)
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

  int validate_start_option_key(VALUE key, VALUE, VALUE data)
  {
    auto *context = reinterpret_cast<OptionValidationContext *>(data);
    if (!SYMBOL_P(key))
    {
      context->valid = false;
      context->invalid_option_name = "non-symbol keyword";
      return ST_STOP;
    }

    const ID key_id = SYM2ID(key);
    if (key_id == id_port ||
        key_id == id_host ||
        key_id == id_workers ||
        key_id == id_threads ||
        key_id == id_queue_capacity ||
        key_id == id_scheduler_policy ||
        key_id == id_max_request_head_bytes ||
        key_id == id_request_timeout ||
        key_id == id_request_head_timeout ||
        key_id == id_first_data_timeout ||
        key_id == id_persistent_timeout ||
        key_id == id_worker_timeout ||
        key_id == id_log_level)
    {
      return ST_CONTINUE;
    }
    context->valid = false;
    context->invalid_option_name = rb_id2name(key_id);
    return ST_STOP;
  }

  VALUE protected_ruby_call_value(VALUE (*func)(VALUE), VALUE data, const char *failure_context)
  {
    int state = 0;
    const VALUE result = rb_protect(func, data, &state);
    if (state == 0)
    {
      return result;
    }

    VALUE message = Qnil;
    int message_state = 0;
    message = rb_protect(protected_format_ruby_exception_message, Qnil, &message_state);
    rb_set_errinfo(Qnil);

    if (message_state != 0 || NIL_P(message))
    {
      throw std::runtime_error(std::string(failure_context) + ": Ruby exception");
    }

    throw std::runtime_error(
        std::string(failure_context) + ": " + StringValueCStr(message));
  }

  long protected_ruby_call_long(VALUE (*func)(VALUE), VALUE data, const char *failure_context)
  {
    return NUM2LONG(protected_ruby_call_value(func, data, failure_context));
  }

  long parse_integer_value(const char *name, const std::string &value, long minimum, long maximum)
  {
    errno = 0;
    char *end = nullptr;
    const long parsed_value = std::strtol(value.c_str(), &end, 10);
    if (errno != 0 || end == value.c_str() || *end != '\0' || parsed_value < minimum || parsed_value > maximum)
    {
      throw std::runtime_error(
          "invalid " + std::string(name) + ": " + value + ". Expected an integer between " +
          std::to_string(minimum) + " and " + std::to_string(maximum) + ".");
    }

    return parsed_value;
  }

  long configured_integer_from_env(
      const char *name,
      long default_value,
      long minimum,
      long maximum,
      const char *extra_guidance = nullptr)
  {
    const char *env_value = std::getenv(name);
    if (env_value == nullptr || env_value[0] == '\0')
    {
      return default_value;
    }

    try
    {
      return parse_integer_value(name, env_value, minimum, maximum);
    }
    catch (const std::runtime_error &error)
    {
      if (extra_guidance == nullptr)
      {
        throw;
      }

      throw std::runtime_error(std::string(error.what()) + " " + extra_guidance);
    }
  }

  long configured_integer_from_ruby(
      VALUE options,
      ID key,
      const char *name,
      long default_value,
      long minimum,
      long maximum)
  {
    if (NIL_P(options))
    {
      return default_value;
    }

    const std::string invalid_option_prefix = std::string("invalid ") + name;
    VALUE lookup_args[2] = {options, ID2SYM(key)};
    const VALUE option_value = protected_ruby_call_value(
        protected_rb_hash_lookup,
        reinterpret_cast<VALUE>(lookup_args),
        "failed to read Ruby start options");
    if (NIL_P(option_value))
    {
      return default_value;
    }

    ProtectedIntegerConversion integer_conversion{option_value, Qnil};
    const VALUE integer_value = protected_ruby_call_value(
        protected_rb_integer,
        reinterpret_cast<VALUE>(&integer_conversion),
        invalid_option_prefix.c_str());
    VALUE integer_value_copy = integer_value;
    const long parsed_value = protected_ruby_call_long(
        protected_rb_num2long,
        reinterpret_cast<VALUE>(&integer_value_copy),
        invalid_option_prefix.c_str());
    if (parsed_value < minimum || parsed_value > maximum)
    {
      ProtectedStringConversion string_conversion{option_value, Qnil};
      VALUE option_string = protected_ruby_call_value(
          protected_rb_obj_as_string,
          reinterpret_cast<VALUE>(&string_conversion),
          invalid_option_prefix.c_str());
      throw std::runtime_error(
          "invalid " + std::string(name) + ": " + StringValueCStr(option_string) +
          ". Expected an integer between " + std::to_string(minimum) + " and " + std::to_string(maximum) + ".");
    }

    return parsed_value;
  }

  void validate_supported_ruby_options(VALUE options)
  {
    if (NIL_P(options))
    {
      return;
    }

    OptionValidationContext context{true, ""};
    rb_hash_foreach(options, validate_start_option_key, reinterpret_cast<VALUE>(&context));
    if (!context.valid)
    {
      throw std::runtime_error("unknown start option: " + context.invalid_option_name);
    }
  }

  std::string configured_string_from_env(const char *name, const std::string &default_value)
  {
    const char *env_value = std::getenv(name);
    if (env_value == nullptr || env_value[0] == '\0')
    {
      return default_value;
    }

    return env_value;
  }

  std::string configured_string_from_ruby(
      VALUE options,
      ID key,
      const char *name,
      const std::string &default_value)
  {
    if (NIL_P(options))
    {
      return default_value;
    }

    VALUE lookup_args[2] = {options, ID2SYM(key)};
    const VALUE option_value = protected_ruby_call_value(
        protected_rb_hash_lookup,
        reinterpret_cast<VALUE>(lookup_args),
        "failed to read Ruby start options");
    if (NIL_P(option_value))
    {
      return default_value;
    }

    ProtectedStringConversion string_conversion{option_value, Qnil};
    VALUE option_string = protected_ruby_call_value(
        protected_rb_obj_as_string,
        reinterpret_cast<VALUE>(&string_conversion),
        ("invalid " + std::string(name)).c_str());
    const char *string_value = StringValueCStr(option_string);
    if (string_value[0] == '\0')
    {
      throw std::runtime_error("invalid " + std::string(name) + ": value must not be empty");
    }

    return string_value;
  }

  std::string trim_ascii_whitespace(std::string value)
  {
    const auto is_not_space = [](unsigned char character) {
      return !std::isspace(character);
    };

    value.erase(
        value.begin(),
        std::find_if(value.begin(), value.end(), is_not_space));
    value.erase(
        std::find_if(value.rbegin(), value.rend(), is_not_space).base(),
        value.end());
    return value;
  }

  std::pair<std::size_t, std::size_t> default_thread_range()
  {
    return {1, 1};
  }

  std::pair<std::size_t, std::size_t> validated_thread_range(long min_threads, long max_threads, const char *name)
  {
    if (min_threads < 1 || max_threads < 1 || min_threads > max_threads)
    {
      throw std::runtime_error(
          "invalid " + std::string(name) + ": expected thread range with 1 <= min <= max");
    }

    return {
        static_cast<std::size_t>(min_threads),
        static_cast<std::size_t>(max_threads)};
  }

  std::pair<std::size_t, std::size_t> configured_threads_from_ruby(VALUE options)
  {
    if (NIL_P(options))
    {
      return default_thread_range();
    }

    VALUE lookup_args[2] = {options, ID2SYM(id_threads)};
    const VALUE option_value = protected_ruby_call_value(
        protected_rb_hash_lookup,
        reinterpret_cast<VALUE>(lookup_args),
        "failed to read Ruby start options");
    if (NIL_P(option_value))
    {
      return default_thread_range();
    }
    if (RB_TYPE_P(option_value, T_ARRAY) == 0)
    {
      throw std::runtime_error("invalid threads option: expected an Array with one or two integer values");
    }

    const long length = RARRAY_LEN(option_value);
    if (length < 1 || length > 2)
    {
      throw std::runtime_error("invalid threads option: expected one or two integer values");
    }

    VALUE min_value = rb_ary_entry(option_value, 0);
    VALUE max_value = length == 2 ? rb_ary_entry(option_value, 1) : min_value;
    ProtectedIntegerConversion min_conversion{min_value, Qnil};
    ProtectedIntegerConversion max_conversion{max_value, Qnil};
    VALUE min_integer = protected_ruby_call_value(
        protected_rb_integer,
        reinterpret_cast<VALUE>(&min_conversion),
        "invalid threads option");
    VALUE max_integer = protected_ruby_call_value(
        protected_rb_integer,
        reinterpret_cast<VALUE>(&max_conversion),
        "invalid threads option");
    VALUE min_copy = min_integer;
    VALUE max_copy = max_integer;
    return validated_thread_range(
        protected_ruby_call_long(protected_rb_num2long, reinterpret_cast<VALUE>(&min_copy), "invalid threads option"),
        protected_ruby_call_long(protected_rb_num2long, reinterpret_cast<VALUE>(&max_copy), "invalid threads option"),
        "threads option");
  }

  std::pair<std::size_t, std::size_t> configured_threads_from_env(
      const std::pair<std::size_t, std::size_t> &default_value)
  {
    const char *env_value = std::getenv("VAJRA_THREADS");
    if (env_value == nullptr || env_value[0] == '\0')
    {
      const char *max_threads_env = std::getenv("MAX_THREADS");
      if (max_threads_env == nullptr || max_threads_env[0] == '\0')
      {
        return default_value;
      }

      const long max_threads = parse_integer_value("MAX_THREADS", max_threads_env, 1, 1'024);
      return validated_thread_range(
          static_cast<long>(default_value.first),
          max_threads,
          "MAX_THREADS");
    }

    std::stringstream stream(env_value);
    std::string first_token;
    std::string second_token;
    if (!std::getline(stream, first_token, ','))
    {
      throw std::runtime_error("invalid VAJRA_THREADS: expected one or two integer values");
    }
    const std::string trimmed_first = trim_ascii_whitespace(first_token);
    if (trimmed_first.empty())
    {
      throw std::runtime_error("invalid VAJRA_THREADS: expected one or two integer values");
    }

    long min_threads = parse_integer_value("VAJRA_THREADS", trimmed_first, 1, 1'024);
    long max_threads = min_threads;
    if (std::getline(stream, second_token, ','))
    {
      const std::string trimmed_second = trim_ascii_whitespace(second_token);
      if (trimmed_second.empty())
      {
        throw std::runtime_error("invalid VAJRA_THREADS: expected one or two integer values");
      }
      max_threads = parse_integer_value("VAJRA_THREADS", trimmed_second, 1, 1'024);
      std::string trailing_token;
      if (std::getline(stream, trailing_token, ','))
      {
        throw std::runtime_error("invalid VAJRA_THREADS: expected one or two integer values");
      }
    }

    return validated_thread_range(min_threads, max_threads, "VAJRA_THREADS");
  }

  std::string normalized_log_level(const std::string &value, const char *name)
  {
    std::string normalized = value;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char character) {
      return static_cast<char>(std::tolower(character));
    });

    if (normalized == "debug" ||
        normalized == "info" ||
        normalized == "warn" ||
        normalized == "error" ||
        normalized == "fatal")
    {
      return normalized;
    }

    throw std::runtime_error(
        "invalid " + std::string(name) + ": " + value +
        ". Expected one of: debug, info, warn, error, fatal.");
  }

  std::string normalized_scheduler_policy(const std::string &value, const char *name)
  {
    std::string normalized = value;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char character) {
      return static_cast<char>(std::tolower(character));
    });

    if (normalized == "least_loaded")
    {
      return normalized;
    }

    throw std::runtime_error(
        "invalid " + std::string(name) + ": " + value +
        ". Expected: least_loaded.");
  }

  bool debug_logging_enabled(const std::string &log_level)
  {
    return log_level == "debug";
  }

  RuntimeConfig configured_runtime(VALUE options)
  {
    validate_supported_ruby_options(options);

    const std::string ruby_host = configured_string_from_ruby(options, id_host, "host option", "0.0.0.0");
    const long ruby_port = configured_integer_from_ruby(options, id_port, "port option", 3000, 0, 65'535);
    const long ruby_workers = configured_integer_from_ruby(options, id_workers, "workers option", 1, 1, 1'024);
    const std::pair<std::size_t, std::size_t> ruby_threads = configured_threads_from_ruby(options);
    const long ruby_max_connections = configured_integer_from_ruby(
        options,
        id_max_connections,
        "max_connections option",
        256,
        1,
        std::numeric_limits<int>::max());
    const long ruby_queue_capacity = configured_integer_from_ruby(
        options,
        id_queue_capacity,
        "queue_capacity option",
        std::numeric_limits<long>::max(),
        1,
        std::numeric_limits<long>::max());
    const std::string ruby_scheduler_policy = normalized_scheduler_policy(
        configured_string_from_ruby(options, id_scheduler_policy, "scheduler_policy option", "least_loaded"),
        "scheduler_policy option");
    const long ruby_max_request_head_bytes = configured_integer_from_ruby(
        options,
        id_max_request_head_bytes,
        "max_request_head_bytes option",
        static_cast<long>(Vajra::request::kDefaultMaxRequestHeadBytes),
        1,
        std::numeric_limits<int>::max());
    const long ruby_request_timeout_seconds = configured_integer_from_ruby(
        options,
        id_request_timeout,
        "request_timeout option",
        25,
        1,
        std::numeric_limits<int>::max());
    const long ruby_request_head_timeout_seconds = configured_integer_from_ruby(
        options,
        id_request_head_timeout,
        "request_head_timeout option",
        5,
        1,
        std::numeric_limits<int>::max());
    const long ruby_first_data_timeout_seconds = configured_integer_from_ruby(
        options,
        id_first_data_timeout,
        "first_data_timeout option",
        30,
        1,
        std::numeric_limits<int>::max());
    const long ruby_persistent_timeout_seconds = configured_integer_from_ruby(
        options,
        id_persistent_timeout,
        "persistent_timeout option",
        30,
        1,
        std::numeric_limits<int>::max());
    const long ruby_worker_timeout_seconds = configured_integer_from_ruby(
        options,
        id_worker_timeout,
        "worker_timeout option",
        60,
        1,
        std::numeric_limits<int>::max());
    const std::string ruby_log_level = normalized_log_level(
        configured_string_from_ruby(options, id_log_level, "log_level option", "info"),
        "log_level option");

    const std::string host = configured_string_from_env("VAJRA_HOST", ruby_host);
    const int port = static_cast<int>(configured_integer_from_env(
        "VAJRA_PORT",
        ruby_port,
        0,
        65'535,
        "Use 0 to request an ephemeral port."));
    const int workers = static_cast<int>(configured_integer_from_env(
        "VAJRA_WORKERS",
        configured_integer_from_env("WEB_CONCURRENCY", ruby_workers, 1, 1'024),
        1,
        1'024));
    const std::pair<std::size_t, std::size_t> threads = configured_threads_from_env(ruby_threads);
    const std::size_t max_connections = static_cast<std::size_t>(ruby_max_connections);
    const std::size_t queue_capacity = static_cast<std::size_t>(configured_integer_from_env(
        "VAJRA_QUEUE_CAPACITY",
        ruby_queue_capacity,
        1,
        std::numeric_limits<long>::max()));
    const std::string scheduler_policy = normalized_scheduler_policy(
        configured_string_from_env("VAJRA_SCHEDULER_POLICY", ruby_scheduler_policy),
        "VAJRA_SCHEDULER_POLICY");

    const std::size_t max_request_head_bytes = static_cast<std::size_t>(configured_integer_from_env(
        "VAJRA_MAX_REQUEST_HEAD_BYTES",
        ruby_max_request_head_bytes,
        1,
        std::numeric_limits<int>::max()));
    const std::size_t request_timeout_seconds = static_cast<std::size_t>(configured_integer_from_env(
        "VAJRA_REQUEST_TIMEOUT",
        ruby_request_timeout_seconds,
        1,
        std::numeric_limits<int>::max()));
    const int request_head_timeout_seconds = static_cast<int>(configured_integer_from_env(
        "VAJRA_REQUEST_HEAD_TIMEOUT",
        ruby_request_head_timeout_seconds,
        1,
        std::numeric_limits<int>::max()));
    const int first_data_timeout_seconds = static_cast<int>(configured_integer_from_env(
        "VAJRA_FIRST_DATA_TIMEOUT",
        ruby_first_data_timeout_seconds,
        1,
        std::numeric_limits<int>::max()));
    const int persistent_timeout_seconds = static_cast<int>(configured_integer_from_env(
        "VAJRA_PERSISTENT_TIMEOUT",
        ruby_persistent_timeout_seconds,
        1,
        std::numeric_limits<int>::max()));
    const int worker_timeout_seconds = static_cast<int>(configured_integer_from_env(
        "VAJRA_WORKER_TIMEOUT",
        ruby_worker_timeout_seconds,
        1,
        std::numeric_limits<int>::max()));
    const std::string log_level = normalized_log_level(
        configured_string_from_env("VAJRA_LOG_LEVEL", ruby_log_level),
        "VAJRA_LOG_LEVEL");

    return RuntimeConfig{
        host,
        port,
        workers,
        threads.first,
        threads.second,
        max_connections,
        queue_capacity,
        scheduler_policy,
        max_request_head_bytes,
        request_timeout_seconds,
        request_head_timeout_seconds,
        first_data_timeout_seconds,
        persistent_timeout_seconds,
        worker_timeout_seconds,
        log_level};
  }

  VALUE rb_vajra_start(int argc, VALUE *argv, VALUE self)
  {
    (void)self;
    try
    {
      VALUE options = Qnil;
      rb_scan_args(argc, argv, "0:", &options);
      const RuntimeConfig config = configured_runtime(options);
      VajraNative::start(
          config.host,
          config.port,
          config.workers,
          config.min_threads,
          config.max_threads,
          config.max_connections,
          config.queue_capacity,
          config.scheduler_policy,
          config.max_request_head_bytes,
          config.request_timeout_seconds,
          config.request_head_timeout_seconds,
          config.first_data_timeout_seconds,
          config.persistent_timeout_seconds,
          config.worker_timeout_seconds,
          config.log_level);
    }
    catch (const std::exception &error)
    {
      raise_ruby_runtime_error("Unable to start Vajra", error);
    }
    return Qnil;
  }

  VALUE rb_vajra_stop(VALUE self)
  {
    (void)self;
    try
    {
      VajraNative::stop();
    }
    catch (const std::exception &error)
    {
      raise_ruby_runtime_error("Unable to stop Vajra", error);
    }
    return Qnil;
  }

  VALUE rb_rack_execution_native_set_callback(VALUE self, VALUE callback)
  {
    (void)self;
    Vajra::rack::set_rack_execution_callback(callback);
    return callback;
  }

  VALUE rb_boot_native_set_callback(VALUE self, VALUE callback)
  {
    (void)self;
    {
      const std::lock_guard<std::mutex> callback_lock(boot_callback_mutex);
      boot_callback = callback;
    }
    return callback;
  }
}

namespace VajraNative
{
  namespace
  {
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

    void set_worker_runtime(pid_t pid, int request_channel_fd)
    {
      const std::lock_guard<std::mutex> lock(server_mutex);
      worker_pids.push_back(pid);
      worker_request_channel_fds.push_back(request_channel_fd);
      worker_startup_in_progress = false;
    }

    void clear_worker_runtime()
    {
      std::vector<int> request_channel_fds;
      const std::lock_guard<std::mutex> lock(server_mutex);
      request_channel_fds = std::move(worker_request_channel_fds);
      worker_pids.clear();
      worker_request_channel_fds.clear();
      stop_requested = false;
      worker_startup_in_progress = false;

      for (int request_channel_fd : request_channel_fds)
      {
        close_fd_if_open(request_channel_fd);
      }
    }

    void install_server_instance(std::shared_ptr<Vajra::Server> server)
    {
      const std::lock_guard<std::mutex> lock(server_mutex);
      server_instance = std::move(server);
      worker_startup_in_progress = false;
    }

    std::shared_ptr<Vajra::Server> take_server_instance()
    {
      const std::lock_guard<std::mutex> lock(server_mutex);
      std::shared_ptr<Vajra::Server> server = server_instance;
      server_instance.reset();
      return server;
    }

    bool try_begin_startup()
    {
      const std::lock_guard<std::mutex> lock(server_mutex);
      if (!worker_pids.empty() || server_instance || worker_startup_in_progress)
      {
        return false;
      }

      worker_startup_in_progress = true;
      return true;
    }

    bool stop_worker_processes()
    {
      std::vector<pid_t> pids;
      bool startup_in_progress = false;
      std::vector<int> request_channel_fds;
      {
        const std::lock_guard<std::mutex> lock(server_mutex);
        pids = worker_pids;
        request_channel_fds = worker_request_channel_fds;
        startup_in_progress = worker_startup_in_progress;
        if (!pids.empty() || startup_in_progress)
        {
          stop_requested = true;
        }
      }

      for (int request_channel_fd : request_channel_fds)
      {
        shutdown(request_channel_fd, SHUT_RDWR);
        close_fd_if_open(request_channel_fd);
      }
      {
        const std::lock_guard<std::mutex> lock(server_mutex);
        worker_request_channel_fds.clear();
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

    void replay_pending_stop_if_needed()
    {
      bool should_stop = false;
      {
        const std::lock_guard<std::mutex> lock(server_mutex);
        should_stop = stop_requested || shutting_down != 0;
      }

      if (should_stop)
      {
        stop_worker_processes();
      }
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

        if (shutdown_requested() && !forwarded_shutdown)
        {
          stop_worker_processes();
          forwarded_shutdown = true;
        }

        if (any_remaining)
        {
          usleep(10'000);
          continue;
        }
      }
    }

    void wait_for_worker_exit(const std::vector<pid_t> &pids)
    {
      WorkerWaitContext context{pids, ""};
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

    void reap_worker_process(pid_t pid)
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

    void reap_worker_processes(const std::vector<pid_t> &pids)
    {
      for (pid_t pid : pids)
      {
        reap_worker_process(pid);
      }
    }

    void run_worker_process(
        std::vector<int> request_channel_fds,
        std::size_t min_threads,
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
        const BootContractResult boot_result = run_boot_contract(
            BootContractConfig{port, max_request_head_bytes, kWorkerBootstrapRuntimeRole});
        if (boot_result.status != BootStatus::ready)
        {
          exit_worker_bootstrap_failure(
              readiness_write_fd,
              diagnostic_for_boot_result_failure(boot_result),
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
        Vajra::rack::run_worker_request_execution_loop(request_channel_fds, min_threads, max_threads);
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

    void run_master_runtime_server(
        const std::string &host,
        int port,
        std::size_t max_request_head_bytes,
        const std::vector<std::vector<int>> &request_channel_fds,
        const std::vector<pid_t> &worker_pids_for_runtime,
        std::size_t min_threads,
        std::size_t max_connections,
        std::size_t queue_capacity,
        std::size_t request_timeout_seconds,
        int request_head_timeout_seconds,
        int first_data_timeout_seconds,
        int persistent_timeout_seconds,
        int worker_timeout_seconds,
        int worker_processes,
        bool debug_logging)
    {
      auto server = std::make_shared<Vajra::Server>(
          port,
          host,
          max_request_head_bytes,
          std::make_shared<Vajra::rack::RackRequestExecutor>(
              Vajra::rack::request_channel_transport(
                  request_channel_fds,
                  std::vector<int>(worker_pids_for_runtime.begin(), worker_pids_for_runtime.end()),
                  min_threads,
                  queue_capacity,
                  request_timeout_seconds,
                  worker_timeout_seconds,
                  debug_logging)),
          kNativeRuntimeControlRole,
          kMasterWorkerMode,
          worker_processes,
          kWorkerBootstrapRuntimeRole,
          debug_logging,
          -1,
          request_head_timeout_seconds,
          first_data_timeout_seconds,
          persistent_timeout_seconds,
          max_connections,
          begin_runtime_shutdown);
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
  }

  bool shutdown_requested()
  {
    return shutting_down != 0;
  }

  void begin_runtime_shutdown()
  {
    ::begin_runtime_shutdown();
  }

  void start(
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
    SignalHandlerGuard signal_handler_guard;
    signal_handler_guard.install();

    shutting_down = 0;
    {
      std::lock_guard<std::mutex> lock(server_mutex);
      runtime_shutdown_started = false;
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
      if (scheduler_policy != "least_loaded")
      {
        throw std::runtime_error("unsupported scheduler_policy: " + scheduler_policy);
      }
      const bool debug_logging = debug_logging_enabled(log_level);
      log_runtime_banner_start(host, port, workers, min_threads, max_threads);
      const BootContractResult master_boot_result = run_boot_contract(
          BootContractConfig{port, max_request_head_bytes, kMasterPreloadRuntimeRole});
      ensure_ready_boot_result(master_boot_result);

      std::vector<pid_t> booted_worker_pids;
      std::vector<std::vector<int>> parent_request_channels;

      for (int worker_index = 0; worker_index < workers; ++worker_index)
      {
        int readiness_pipe[2] = {-1, -1};
        if (pipe(readiness_pipe) != 0)
        {
          const int error_number = errno;
          throw std::runtime_error(
              std::string("worker bootstrap pipe creation failed: ") + std::strerror(error_number));
        }

        std::vector<std::array<int, 2>> request_channels;
        request_channels.reserve(max_threads);
        for (std::size_t thread_index = 0; thread_index < max_threads; ++thread_index)
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
              min_threads,
              max_threads,
              port,
              max_request_head_bytes,
              readiness_pipe[1],
              worker_index,
              workers,
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
          const std::lock_guard<std::mutex> lock(server_mutex);
          for (std::size_t index = 1; index < worker_parent_channels.size(); ++index)
          {
            worker_request_channel_fds.push_back(worker_parent_channels[index]);
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

      run_master_runtime_server(
          host,
          port,
          max_request_head_bytes,
          parent_request_channels,
          booted_worker_pids,
          min_threads,
          max_connections,
          queue_capacity,
          request_timeout_seconds,
          request_head_timeout_seconds,
          first_data_timeout_seconds,
          persistent_timeout_seconds,
          worker_timeout_seconds,
          workers,
          debug_logging);
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

  void stop()
  {
    begin_runtime_shutdown();
    (void)stop_worker_processes();

    Vajra::Server *server = nullptr;
    std::shared_ptr<Vajra::Server> server_handle;
    {
      std::lock_guard<std::mutex> lock(server_mutex);
      server_handle = server_instance;
      server = server_handle.get();
    }

    if (server != nullptr)
    {
      server->stop();
    }
  }
}

extern "C" void Init_vajra()
{
  id_port = rb_intern("port");
  id_host = rb_intern("host");
  id_workers = rb_intern("workers");
  id_threads = rb_intern("threads");
  id_max_connections = rb_intern("max_connections");
  id_queue_capacity = rb_intern("queue_capacity");
  id_scheduler_policy = rb_intern("scheduler_policy");
  id_request_timeout = rb_intern("request_timeout");
  id_request_head_timeout = rb_intern("request_head_timeout");
  id_first_data_timeout = rb_intern("first_data_timeout");
  id_persistent_timeout = rb_intern("persistent_timeout");
  id_worker_timeout = rb_intern("worker_timeout");
  id_max_request_head_bytes = rb_intern("max_request_head_bytes");
  id_log_level = rb_intern("log_level");
  id_runtime_role = rb_intern("runtime_role");
  rb_global_variable(&boot_callback);
  Vajra::rack::initialize_rack_execution_bridge();
  VALUE mVajra = rb_define_module("Vajra");
  VALUE mInternal = rb_define_module_under(mVajra, "Internal");
  VALUE mBoot = rb_define_module_under(mInternal, "Boot");
  VALUE mRackExecution = rb_define_module_under(mInternal, "RackExecution");
  rb_define_singleton_method(mVajra, "start", RUBY_METHOD_FUNC(rb_vajra_start), -1);
  rb_define_singleton_method(mVajra, "stop", RUBY_METHOD_FUNC(rb_vajra_stop), 0);
  rb_define_singleton_method(
      mBoot,
      "__native_set_boot_callback__",
      RUBY_METHOD_FUNC(rb_boot_native_set_callback),
      1);
  rb_define_singleton_method(
      mRackExecution,
      "__native_set_callback__",
      RUBY_METHOD_FUNC(rb_rack_execution_native_set_callback),
      1);
}
