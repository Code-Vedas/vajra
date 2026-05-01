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
#include <stdexcept>
#include <cstring>
#include <string>

namespace
{
  volatile std::sig_atomic_t shutting_down = 0;
  std::mutex server_mutex;
  std::unique_ptr<Server> server_instance;

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

  int configured_listener_port()
  {
    const char *port_value = std::getenv("VAJRA_PORT");
    if (port_value == nullptr || port_value[0] == '\0')
    {
      return 3000;
    }

    errno = 0;
    char *end = nullptr;
    const long port = std::strtol(port_value, &end, 10);
    if (errno != 0 || end == port_value || *end != '\0' || port < 0 || port > 65'535)
    {
      throw std::runtime_error(
          std::string("invalid VAJRA_PORT: ") + port_value +
          ". Expected an integer between 0 and 65535. Use 0 to request an ephemeral port.");
    }

    return static_cast<int>(port);
  }

  VALUE rb_vajra_start(VALUE self)
  {
    (void)self;
    try
    {
      VajraNative::start();
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

  void start()
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
        server_instance = std::make_unique<Server>(configured_listener_port());
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
  VALUE mVajra = rb_define_module("Vajra");
  rb_define_singleton_method(mVajra, "start", RUBY_METHOD_FUNC(rb_vajra_start), 0);
  rb_define_singleton_method(mVajra, "stop", RUBY_METHOD_FUNC(rb_vajra_stop), 0);
}
