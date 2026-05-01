// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "vajra.hpp"
#include "ruby.h"
#include "ruby/thread.h"

#include <csignal>
#include <cstdlib>
#include <cerrno>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <limits>
#include <stdexcept>
#include <cstring>
#include <string>

namespace
{
  volatile std::sig_atomic_t shutting_down = 0;
  std::mutex server_mutex;
  std::unique_ptr<Server> server_instance;
  ID id_port;
  ID id_max_request_head_bytes;

  struct RuntimeConfig
  {
    int port;
    std::size_t max_request_head_bytes;
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

  struct ServerStartContext
  {
    Server *server;
    std::exception_ptr error;
  };

  void *run_server_without_gvl(void *data)
  {
    auto *context = static_cast<ServerStartContext *>(data);

    try
    {
      context->server->start();
    }
    catch (...)
    {
      context->error = std::current_exception();
    }

    return nullptr;
  }

  [[noreturn]] void raise_ruby_runtime_error(const char *prefix, const std::exception &error)
  {
    rb_raise(rb_eRuntimeError, "%s: %s", prefix, error.what());
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

    const VALUE option_value = rb_hash_lookup(options, ID2SYM(key));
    if (NIL_P(option_value))
    {
      return default_value;
    }

    const VALUE integer_value = rb_Integer(option_value);
    const long parsed_value = NUM2LONG(integer_value);
    if (parsed_value < minimum || parsed_value > maximum)
    {
      VALUE option_string = rb_obj_as_string(option_value);
      throw std::runtime_error(
          "invalid " + std::string(name) + ": " + StringValueCStr(option_string) +
          ". Expected an integer between " + std::to_string(minimum) + " and " + std::to_string(maximum) + ".");
    }

    return parsed_value;
  }

  RuntimeConfig configured_runtime(VALUE options)
  {
    const long ruby_port = configured_integer_from_ruby(options, id_port, "start port", 3000, 0, 65'535);
    const long ruby_max_request_head_bytes = configured_integer_from_ruby(
        options,
        id_max_request_head_bytes,
        "start max_request_head_bytes",
        static_cast<long>(kDefaultMaxRequestHeadBytes),
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
}

namespace VajraNative
{
  bool shutdown_requested()
  {
    return shutting_down != 0;
  }

  void start(int port, std::size_t max_request_head_bytes)
  {
    SignalHandlerGuard signal_handler_guard;
    signal_handler_guard.install();

    try
    {
      Server *server = nullptr;
      {
        std::lock_guard<std::mutex> lock(server_mutex);
        if (server_instance)
        {
          std::cout << "Vajra already running" << std::endl;
          return;
        }

        shutting_down = 0;
        server_instance = std::make_unique<Server>(port, max_request_head_bytes);
        server = server_instance.get();
      }

      ServerStartContext context{server, nullptr};
      rb_thread_call_without_gvl(run_server_without_gvl, &context, RUBY_UBF_IO, nullptr);
      if (context.error)
      {
        std::rethrow_exception(context.error);
      }
    }
    catch (...)
    {
      std::lock_guard<std::mutex> lock(server_mutex);
      server_instance.reset();
      throw;
    }

    std::lock_guard<std::mutex> lock(server_mutex);
    server_instance.reset();
  }

  void stop()
  {
    std::lock_guard<std::mutex> lock(server_mutex);
    if (!server_instance)
    {
      return;
    }

    server_instance->stop();
  }
}

extern "C" void Init_vajra()
{
  id_port = rb_intern("port");
  id_max_request_head_bytes = rb_intern("max_request_head_bytes");
  VALUE mVajra = rb_define_module("Vajra");
  rb_define_singleton_method(mVajra, "start", RUBY_METHOD_FUNC(rb_vajra_start), -1);
  rb_define_singleton_method(mVajra, "stop", RUBY_METHOD_FUNC(rb_vajra_stop), 0);
}
