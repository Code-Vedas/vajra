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
#include <cstdlib>
#include <cerrno>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <limits>
#include <optional>
#include <stdexcept>
#include <cstring>
#include <cstdint>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace
{
  volatile std::sig_atomic_t shutting_down = 0;
  std::mutex server_mutex;
  std::unique_ptr<Vajra::Server> server_instance;
  pid_t worker_pid = -1;
  bool stop_requested = false;
  ID id_port;
  ID id_max_request_head_bytes;
  ID id_runtime_role;
  std::mutex boot_callback_mutex;
  VALUE boot_callback = Qnil;
  constexpr const char *kMasterPreloadRuntimeRole = "ruby_master_preload";
  constexpr const char *kWorkerBootstrapRuntimeRole = "ruby_worker_bootstrap";
  constexpr const char *kMasterWorkerMode = "master_worker";
  constexpr const char *kSingleProcessRuntimeRole = "single_process_bootstrap";
  constexpr const char *kSingleProcessMode = "single_process";
  constexpr int kWorkerProcessCount = 1;
  constexpr std::size_t kMaxWorkerBootstrapStringPayloadBytes = 64 * 1024;

  struct RuntimeConfig
  {
    int port;
    std::size_t max_request_head_bytes;
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
    pid_t pid;
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
    const std::uint32_t length = static_cast<std::uint32_t>(value.size());
    write_all_or_throw(fd, &length, sizeof(length));
    if (length > 0)
    {
      write_all_or_throw(fd, value.data(), length);
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
    if (key_id == id_port || key_id == id_max_request_head_bytes)
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

  RuntimeConfig configured_runtime(VALUE options)
  {
    validate_supported_ruby_options(options);

    const long ruby_port = configured_integer_from_ruby(options, id_port, "port option", 3000, 0, 65'535);
    const long ruby_max_request_head_bytes = configured_integer_from_ruby(
        options,
        id_max_request_head_bytes,
        "max_request_head_bytes option",
        static_cast<long>(Vajra::request::kDefaultMaxRequestHeadBytes),
        1,
        std::numeric_limits<int>::max());

    const int port = static_cast<int>(configured_integer_from_env(
        "VAJRA_PORT",
        ruby_port,
        0,
        65'535,
        "Use 0 to request an ephemeral port."));

    const std::size_t max_request_head_bytes = static_cast<std::size_t>(configured_integer_from_env(
        "VAJRA_MAX_REQUEST_HEAD_BYTES",
        ruby_max_request_head_bytes,
        1,
        std::numeric_limits<int>::max()));

    return RuntimeConfig{port, max_request_head_bytes};
  }

  VALUE rb_vajra_start(int argc, VALUE *argv, VALUE self)
  {
    (void)self;
    try
    {
      VALUE options = Qnil;
      rb_scan_args(argc, argv, "0:", &options);
      const RuntimeConfig config = configured_runtime(options);
      VajraNative::start(config.port, config.max_request_head_bytes);
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

    void set_worker_pid(pid_t pid)
    {
      const std::lock_guard<std::mutex> lock(server_mutex);
      worker_pid = pid;
    }

    void clear_worker_pid()
    {
      const std::lock_guard<std::mutex> lock(server_mutex);
      worker_pid = -1;
      stop_requested = false;
    }

    pid_t current_worker_pid()
    {
      const std::lock_guard<std::mutex> lock(server_mutex);
      return worker_pid;
    }

    void install_single_process_server(std::unique_ptr<Vajra::Server> server)
    {
      const std::lock_guard<std::mutex> lock(server_mutex);
      server_instance = std::move(server);
    }

    std::unique_ptr<Vajra::Server> take_single_process_server()
    {
      const std::lock_guard<std::mutex> lock(server_mutex);
      return std::move(server_instance);
    }

    bool single_process_server_running()
    {
      const std::lock_guard<std::mutex> lock(server_mutex);
      return static_cast<bool>(server_instance);
    }

    bool stop_worker_process()
    {
      pid_t pid = -1;
      {
        const std::lock_guard<std::mutex> lock(server_mutex);
        stop_requested = true;
        pid = worker_pid;
      }
      if (pid <= 0)
      {
        return false;
      }

      if (kill(pid, SIGINT) != 0 && errno != ESRCH)
      {
        throw std::runtime_error("failed to signal worker shutdown");
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
        stop_worker_process();
      }
    }

    void *wait_for_worker_exit_without_gvl(void *data)
    {
      auto *context = static_cast<WorkerWaitContext *>(data);
      bool forwarded_shutdown = false;
      for (;;)
      {
        int status = 0;
        const pid_t wait_result = waitpid(context->pid, &status, 0);
        if (wait_result == context->pid)
        {
          if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
          {
            return nullptr;
          }

          if (WIFEXITED(status))
          {
            context->error_message =
                "worker process exited unexpectedly with status " + std::to_string(WEXITSTATUS(status));
            return nullptr;
          }

          if (WIFSIGNALED(status))
          {
            context->error_message =
                "worker process exited unexpectedly due to signal " + std::to_string(WTERMSIG(status));
            return nullptr;
          }

          context->error_message = "worker process exited unexpectedly";
          return nullptr;
        }

        if (wait_result < 0 && errno == EINTR)
        {
          if (shutdown_requested() && !forwarded_shutdown)
          {
            stop_worker_process();
            forwarded_shutdown = true;
          }
          continue;
        }

        context->error_message = "failed to wait for worker process";
        return nullptr;
      }
    }

    void wait_for_worker_exit(pid_t pid)
    {
      WorkerWaitContext context{pid, ""};
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

    void run_worker_process(
        int listener_fd,
        int port,
        std::size_t max_request_head_bytes,
        int readiness_write_fd)
    {
      try
      {
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
        close(readiness_write_fd);

        Vajra::Server server(
            port,
            max_request_head_bytes,
            std::make_shared<Vajra::rack::RackRequestExecutor>(),
            boot_result.runtime_role,
            kMasterWorkerMode,
            kWorkerProcessCount,
            listener_fd);
        ServerRunContext context{&server, ""};
        rb_thread_call_without_gvl(
            run_server_without_gvl,
            &context,
            RUBY_UBF_IO,
            nullptr);
        if (!context.error_message.empty())
        {
          throw std::runtime_error(context.error_message);
        }
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

    void run_single_process_server(int port, std::size_t max_request_head_bytes)
    {
      const BootContractResult boot_result = run_boot_contract(
          BootContractConfig{port, max_request_head_bytes, kSingleProcessRuntimeRole});
      ensure_ready_boot_result(boot_result);

      auto server = std::make_unique<Vajra::Server>(
          port,
          max_request_head_bytes,
          std::make_shared<Vajra::rack::RackRequestExecutor>(),
          boot_result.runtime_role,
          kSingleProcessMode,
          0);
      Vajra::Server *server_ptr = server.get();
      install_single_process_server(std::move(server));
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
        auto owned_server = take_single_process_server();
        if (owned_server)
        {
          owned_server->stop();
        }
        throw;
      }

      auto owned_server = take_single_process_server();
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

  void start(int port, std::size_t max_request_head_bytes)
  {
    SignalHandlerGuard signal_handler_guard;
    signal_handler_guard.install();

    shutting_down = 0;

    try
    {
      if (current_worker_pid() > 0 || single_process_server_running())
      {
        std::cout << "Vajra already running" << std::endl;
        return;
      }

      if (!start_called_from_ruby_main_thread())
      {
        run_single_process_server(port, max_request_head_bytes);
        return;
      }

      const BootContractResult master_boot_result = run_boot_contract(
          BootContractConfig{port, max_request_head_bytes, kMasterPreloadRuntimeRole});
      ensure_ready_boot_result(master_boot_result);
      {
        const std::lock_guard<std::mutex> lock(server_mutex);
        stop_requested = false;
      }

      Vajra::listener::Socket listener_socket;
      Vajra::listener::SocketBinding binding = listener_socket.open(port);

      int readiness_pipe[2] = {-1, -1};
      if (pipe(readiness_pipe) != 0)
      {
        const int error_number = errno;
        close_fd_if_open(binding.fd);
        throw std::runtime_error(
            std::string("worker bootstrap pipe creation failed: ") + std::strerror(error_number));
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
        close_fd_if_open(binding.fd);
        throw std::runtime_error(
            std::string("worker fork failed: ") + std::strerror(error_number));
      }

      if (pid == 0)
      {
        close_fd_if_open(readiness_pipe[0]);
        run_worker_process(binding.fd, binding.port, max_request_head_bytes, readiness_pipe[1]);
      }

      set_worker_pid(pid);
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
        close_fd_if_open(binding.fd);
        stop_worker_process();
        reap_worker_process(pid);
        clear_worker_pid();
        throw;
      }
      close_fd_if_open(readiness_pipe[0]);

      if (report.status == WorkerBootstrapStatus::failed)
      {
        close_fd_if_open(binding.fd);
        reap_worker_process(pid);
        clear_worker_pid();
        const auto &diagnostic = report.diagnostic.value();
        throw std::runtime_error(
            "Ruby worker boot failed (" + diagnostic.code + "/" + diagnostic.category + "): " +
            diagnostic.message);
      }

      close_fd_if_open(binding.fd);
      wait_for_worker_exit(pid);
      clear_worker_pid();
    }
    catch (...)
    {
      clear_worker_pid();
      throw;
    }
  }

  void stop()
  {
    if (stop_worker_process())
    {
      return;
    }

    std::lock_guard<std::mutex> lock(server_mutex);
    if (server_instance)
    {
      server_instance->stop();
    }
  }
}

extern "C" void Init_vajra()
{
  id_port = rb_intern("port");
  id_max_request_head_bytes = rb_intern("max_request_head_bytes");
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
