// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "vajra.hpp"
#include "ruby.h"

#include <atomic>
#include <csignal>
#include <iostream>
#include <memory>

namespace
{
  std::atomic<bool> shutting_down{false};
  std::unique_ptr<Server> server_instance;

  void handle_signal(int sig)
  {
    if (sig == SIGINT || sig == SIGTERM)
    {
      shutting_down.store(true);
      VajraNative::stop();
    }
  }

  VALUE rb_vajra_start(VALUE self)
  {
    (void)self;
    VajraNative::start();
    return Qnil;
  }

  VALUE rb_vajra_stop(VALUE self)
  {
    (void)self;
    VajraNative::stop();
    return Qnil;
  }
}

namespace VajraNative
{
  void start()
  {
    if (server_instance)
    {
      std::cout << "Vajra already running" << std::endl;
      return;
    }

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    server_instance = std::make_unique<Server>(3000);
    server_instance->start();
    server_instance.reset();
  }

  void stop()
  {
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
