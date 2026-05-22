// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "vajra.hpp"

#include "rack/rack_request_executor.hpp"
#include "runtime/boot_contract.hpp"
#include "runtime/runtime_config.hpp"
#include "ruby.h"

#include <exception>
#include <string>

namespace
{
  [[noreturn]] void raise_ruby_runtime_error(const char *context, const std::exception &error)
  {
    rb_raise(rb_eRuntimeError, "%s: %s", context, error.what());
  }

  VALUE rb_vajra_start(int argc, VALUE *argv, VALUE self)
  {
    (void)self;
    try
    {
      VALUE options = Qnil;
      rb_scan_args(argc, argv, "0:", &options);
      const Vajra::runtime::RuntimeConfig config = Vajra::runtime::RuntimeConfigLoader::configured_runtime(options);
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
    Vajra::runtime::BootContract::set_callback(callback);
    return callback;
  }
}

extern "C" void Init_vajra()
{
  Vajra::runtime::RuntimeConfigLoader::initialize_ids();
  Vajra::runtime::BootContract::initialize_ids();
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
