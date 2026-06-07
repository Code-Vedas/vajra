// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "runtime/runtime_config.hpp"

#include "request/request_head_error.hpp"
#include "runtime/ruby_support.hpp"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace
{
  ID id_port;
  ID id_host;
  ID id_workers;
  ID id_threads;
  ID id_max_request_head_bytes;
  ID id_max_connections;
  ID id_socket_queue_capacity;
  ID id_request_timeout;
  ID id_request_head_timeout;
  ID id_first_data_timeout;
  ID id_persistent_timeout;
  ID id_worker_timeout;
  ID id_log_level;
  ID id_access_log;
  ID id_error_log;
  ID id_structured_logs;
  ID id_access_log_format;
  ID id_stats_path;
  ID id_metrics_endpoint;
  ID id_trace_enabled;
  ID id_trace_endpoint;
  ID id_trace_service_name;
  ID id_trace_otel_owner;

  struct OptionValidationContext
  {
    bool valid;
    std::string invalid_option_name;
  };

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
        key_id == id_max_connections ||
        key_id == id_socket_queue_capacity ||
        key_id == id_max_request_head_bytes ||
        key_id == id_request_timeout ||
        key_id == id_request_head_timeout ||
        key_id == id_first_data_timeout ||
        key_id == id_persistent_timeout ||
        key_id == id_worker_timeout ||
        key_id == id_log_level ||
        key_id == id_access_log ||
        key_id == id_error_log ||
        key_id == id_structured_logs ||
        key_id == id_access_log_format ||
        key_id == id_stats_path ||
        key_id == id_metrics_endpoint ||
        key_id == id_trace_enabled ||
        key_id == id_trace_endpoint ||
        key_id == id_trace_service_name ||
        key_id == id_trace_otel_owner)
    {
      return ST_CONTINUE;
    }

    context->valid = false;
    context->invalid_option_name = rb_id2name(key_id);
    return ST_STOP;
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
    const VALUE option_value = Vajra::runtime::protected_ruby_call_value(
        Vajra::runtime::protected_rb_hash_lookup,
        reinterpret_cast<VALUE>(lookup_args),
        "failed to read Ruby start options");
    if (NIL_P(option_value))
    {
      return default_value;
    }

    Vajra::runtime::ProtectedIntegerConversion integer_conversion{option_value, Qnil};
    const VALUE integer_value = Vajra::runtime::protected_ruby_call_value(
        Vajra::runtime::protected_rb_integer,
        reinterpret_cast<VALUE>(&integer_conversion),
        invalid_option_prefix.c_str());
    VALUE integer_value_copy = integer_value;
    const long parsed_value = Vajra::runtime::protected_ruby_call_long(
        Vajra::runtime::protected_rb_num2long,
        reinterpret_cast<VALUE>(&integer_value_copy),
        invalid_option_prefix.c_str());
    if (parsed_value < minimum || parsed_value > maximum)
    {
      Vajra::runtime::ProtectedStringConversion string_conversion{option_value, Qnil};
      VALUE option_string = Vajra::runtime::protected_ruby_call_value(
          Vajra::runtime::protected_rb_obj_as_string,
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

  std::string configured_string_from_env(const char *name, const std::string &default_value)
  {
    const char *env_value = std::getenv(name);
    if (env_value == nullptr || env_value[0] == '\0')
    {
      return default_value;
    }

    const std::string trimmed_value = trim_ascii_whitespace(env_value);
    return trimmed_value.empty() ? default_value : trimmed_value;
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
    const VALUE option_value = Vajra::runtime::protected_ruby_call_value(
        Vajra::runtime::protected_rb_hash_lookup,
        reinterpret_cast<VALUE>(lookup_args),
        "failed to read Ruby start options");
    if (NIL_P(option_value))
    {
      return default_value;
    }

    Vajra::runtime::ProtectedStringConversion string_conversion{option_value, Qnil};
    VALUE option_string = Vajra::runtime::protected_ruby_call_value(
        Vajra::runtime::protected_rb_obj_as_string,
        reinterpret_cast<VALUE>(&string_conversion),
        ("invalid " + std::string(name)).c_str());
    const char *string_value = StringValueCStr(option_string);
    if (string_value[0] == '\0')
    {
      throw std::runtime_error("invalid " + std::string(name) + ": value must not be empty");
    }

    return string_value;
  }

  bool configured_boolean_from_ruby(VALUE options, ID key, bool default_value)
  {
    if (NIL_P(options))
    {
      return default_value;
    }

    VALUE lookup_args[2] = {options, ID2SYM(key)};
    const VALUE option_value = Vajra::runtime::protected_ruby_call_value(
        Vajra::runtime::protected_rb_hash_lookup,
        reinterpret_cast<VALUE>(lookup_args),
        "failed to read Ruby start options");
    if (NIL_P(option_value))
    {
      return default_value;
    }

    if (option_value == Qtrue)
    {
      return true;
    }
    if (option_value == Qfalse)
    {
      return false;
    }

    throw std::runtime_error(
        "invalid boolean option: expected true or false for " + std::string(rb_id2name(key)));
  }

  bool configured_boolean_from_env(const char *name, bool default_value)
  {
    std::string value = configured_string_from_env(name, default_value ? "true" : "false");
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
      return static_cast<char>(std::tolower(character));
    });

    if (value == "true" || value == "1" || value == "yes" || value == "on")
    {
      return true;
    }
    if (value == "false" || value == "0" || value == "no" || value == "off")
    {
      return false;
    }

    throw std::runtime_error(
        "invalid " + std::string(name) + ": " + value +
        ". Expected one of: true, false, 1, 0, yes, no, on, off.");
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
    const VALUE option_value = Vajra::runtime::protected_ruby_call_value(
        Vajra::runtime::protected_rb_hash_lookup,
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
    Vajra::runtime::ProtectedIntegerConversion min_conversion{min_value, Qnil};
    Vajra::runtime::ProtectedIntegerConversion max_conversion{max_value, Qnil};
    VALUE min_integer = Vajra::runtime::protected_ruby_call_value(
        Vajra::runtime::protected_rb_integer,
        reinterpret_cast<VALUE>(&min_conversion),
        "invalid threads option");
    VALUE max_integer = Vajra::runtime::protected_ruby_call_value(
        Vajra::runtime::protected_rb_integer,
        reinterpret_cast<VALUE>(&max_conversion),
        "invalid threads option");
    VALUE min_copy = min_integer;
    VALUE max_copy = max_integer;
    return validated_thread_range(
        Vajra::runtime::protected_ruby_call_long(
            Vajra::runtime::protected_rb_num2long,
            reinterpret_cast<VALUE>(&min_copy),
            "invalid threads option"),
        Vajra::runtime::protected_ruby_call_long(
            Vajra::runtime::protected_rb_num2long,
            reinterpret_cast<VALUE>(&max_copy),
            "invalid threads option"),
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

}

void Vajra::runtime::RuntimeConfigLoader::initialize_ids()
{
  id_port = rb_intern("port");
  id_host = rb_intern("host");
  id_workers = rb_intern("workers");
  id_threads = rb_intern("threads");
  id_max_connections = rb_intern("max_connections");
  id_socket_queue_capacity = rb_intern("socket_queue_capacity");
  id_request_timeout = rb_intern("request_timeout");
  id_request_head_timeout = rb_intern("request_head_timeout");
  id_first_data_timeout = rb_intern("first_data_timeout");
  id_persistent_timeout = rb_intern("persistent_timeout");
  id_worker_timeout = rb_intern("worker_timeout");
  id_max_request_head_bytes = rb_intern("max_request_head_bytes");
  id_log_level = rb_intern("log_level");
  id_access_log = rb_intern("access_log");
  id_error_log = rb_intern("error_log");
  id_structured_logs = rb_intern("structured_logs");
  id_access_log_format = rb_intern("access_log_format");
  id_stats_path = rb_intern("stats_path");
  id_metrics_endpoint = rb_intern("metrics_endpoint");
  id_trace_enabled = rb_intern("trace_enabled");
  id_trace_endpoint = rb_intern("trace_endpoint");
  id_trace_service_name = rb_intern("trace_service_name");
  id_trace_otel_owner = rb_intern("trace_otel_owner");
}

Vajra::runtime::RuntimeConfig Vajra::runtime::RuntimeConfigLoader::configured_runtime(VALUE options)
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
  const long ruby_socket_queue_capacity = configured_integer_from_ruby(
      options,
      id_socket_queue_capacity,
      "socket_queue_capacity option",
      256,
      1,
      std::numeric_limits<long>::max());
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
  const std::string ruby_access_log = configured_string_from_ruby(
      options,
      id_access_log,
      "access_log option",
      "");
  const std::string ruby_error_log = configured_string_from_ruby(
      options,
      id_error_log,
      "error_log option",
      "");
  const bool ruby_structured_logs = configured_boolean_from_ruby(options, id_structured_logs, false);
  const std::string ruby_access_log_format = configured_string_from_ruby(
      options,
      id_access_log_format,
      "access_log_format option",
      "");
  const std::string ruby_stats_path = configured_string_from_ruby(
      options,
      id_stats_path,
      "stats_path option",
      "");
  const std::string ruby_metrics_endpoint = configured_string_from_ruby(
      options,
      id_metrics_endpoint,
      "metrics_endpoint option",
      "");
  const bool ruby_trace_enabled = configured_boolean_from_ruby(options, id_trace_enabled, false);
  const std::string ruby_trace_endpoint = configured_string_from_ruby(
      options,
      id_trace_endpoint,
      "trace_endpoint option",
      "");
  const std::string ruby_trace_service_name = configured_string_from_ruby(
      options,
      id_trace_service_name,
      "trace_service_name option",
      "vajra");
  const bool ruby_trace_otel_owner = configured_boolean_from_ruby(options, id_trace_otel_owner, false);

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
  const std::size_t socket_queue_capacity = static_cast<std::size_t>(configured_integer_from_env(
      "VAJRA_SOCKET_QUEUE_CAPACITY",
      ruby_socket_queue_capacity,
      1,
      std::numeric_limits<long>::max()));
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
  const std::string access_log = configured_string_from_env("VAJRA_ACCESS_LOG", ruby_access_log);
  const std::string error_log = configured_string_from_env("VAJRA_ERROR_LOG", ruby_error_log);
  const bool structured_logs = configured_boolean_from_env("VAJRA_STRUCTURED_LOGS", ruby_structured_logs);
  const std::string access_log_format = configured_string_from_env("VAJRA_ACCESS_LOG_FORMAT", ruby_access_log_format);
  const std::string stats_path = configured_string_from_env("VAJRA_STATS_PATH", ruby_stats_path);
  const std::string metrics_endpoint = configured_string_from_env("VAJRA_METRICS_ENDPOINT", ruby_metrics_endpoint);
  const bool trace_enabled = configured_boolean_from_env("VAJRA_TRACE_ENABLED", ruby_trace_enabled);
  const std::string trace_endpoint = configured_string_from_env("VAJRA_TRACE_ENDPOINT", ruby_trace_endpoint);
  const std::string trace_service_name =
      configured_string_from_env("VAJRA_TRACE_SERVICE_NAME", ruby_trace_service_name);
  const bool trace_otel_owner = configured_boolean_from_env("VAJRA_TRACE_OTEL_OWNER", ruby_trace_otel_owner);

  return RuntimeConfig{
      host,
      port,
      workers,
      threads.first,
      threads.second,
      max_connections,
      socket_queue_capacity,
      max_request_head_bytes,
      request_timeout_seconds,
      request_head_timeout_seconds,
      first_data_timeout_seconds,
      persistent_timeout_seconds,
      worker_timeout_seconds,
      log_level,
      access_log,
      error_log,
      structured_logs,
      access_log_format,
      stats_path,
      metrics_endpoint,
      trace_enabled,
      trace_endpoint,
      trace_service_name,
      trace_otel_owner};
}
