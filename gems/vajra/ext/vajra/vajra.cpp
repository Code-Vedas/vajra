// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "vajra.hpp"

#include "rack/rack_request_executor.hpp"
#include "runtime/boot_contract.hpp"
#include "runtime/runtime_config.hpp"
#include "runtime/runtime_logging.hpp"
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
          config.socket_queue_capacity,
          config.max_request_head_bytes,
          config.request_timeout_seconds,
          config.request_head_timeout_seconds,
          config.first_data_timeout_seconds,
          config.persistent_timeout_seconds,
          config.worker_timeout_seconds,
          config.log_level,
          config.access_log,
          config.error_log,
          config.structured_logs,
          config.stats_path,
          config.metrics_endpoint,
          config.trace_enabled,
          config.trace_endpoint,
          config.trace_service_name);
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

  VALUE rb_rack_execution_native_set_app(VALUE self, VALUE app)
  {
    (void)self;
    Vajra::rack::set_rack_execution_app(app);
    return app;
  }

  VALUE rb_boot_native_set_callback(VALUE self, VALUE callback)
  {
    (void)self;
    Vajra::runtime::BootContract::set_callback(callback);
    return callback;
  }

  VALUE rb_tracing_native_set_lifecycle_callback(VALUE self, VALUE callback)
  {
    (void)self;
    Vajra::runtime::set_runtime_lifecycle_callback(reinterpret_cast<void *>(callback));
    return callback;
  }

  VALUE rb_tracing_native_set_status(
      VALUE self,
      VALUE enabled,
      VALUE available,
      VALUE endpoint,
      VALUE service_name)
  {
    (void)self;
    Vajra::runtime::configure_runtime_tracing(
        enabled == Qtrue,
        NIL_P(endpoint) ? std::string() : StringValueCStr(endpoint),
        NIL_P(service_name) ? std::string("vajra") : StringValueCStr(service_name));
    Vajra::runtime::set_runtime_tracing_available(available == Qtrue);
    return Qnil;
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
  VALUE mTracing = rb_define_module_under(mInternal, "Tracing");

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
  rb_define_singleton_method(
      mRackExecution,
      "__native_set_app__",
      RUBY_METHOD_FUNC(rb_rack_execution_native_set_app),
      1);
  rb_define_singleton_method(
      mTracing,
      "__native_set_lifecycle_callback__",
      RUBY_METHOD_FUNC(rb_tracing_native_set_lifecycle_callback),
      1);
  rb_define_singleton_method(
      mTracing,
      "__native_set_tracing_status__",
      RUBY_METHOD_FUNC(rb_tracing_native_set_status),
      4);
}
