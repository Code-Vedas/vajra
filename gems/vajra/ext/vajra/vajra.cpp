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
#include <vector>

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
          config.access_log_format,
          config.stats_path,
          config.metrics_endpoint,
          config.trace_enabled,
          config.trace_endpoint,
          config.trace_service_name,
          config.trace_otel_owner);
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

  VALUE rb_tracing_native_set_request_observability_callback(VALUE self, VALUE callback)
  {
    (void)self;
    Vajra::runtime::set_runtime_request_observability_callback(reinterpret_cast<void *>(callback));
    return callback;
  }

  void hash_set_string(VALUE hash, const char *name, const std::string &value)
  {
    rb_hash_aset(hash, ID2SYM(rb_intern(name)), rb_str_new(value.c_str(), static_cast<long>(value.size())));
  }

  VALUE request_observability_event_to_hash(const Vajra::runtime::RequestObservabilityEvent &queued)
  {
    const Vajra::runtime::AccessLogEvent &access = queued.access;
    VALUE event = rb_hash_new();
    hash_set_string(event, "method", access.method);
    hash_set_string(event, "target", access.target);
    rb_hash_aset(event, ID2SYM(rb_intern("status")), INT2NUM(access.status_code));
    rb_hash_aset(event, ID2SYM(rb_intern("duration_nanoseconds")), LL2NUM(access.duration_nanoseconds));
    rb_hash_aset(event, ID2SYM(rb_intern("bytes_written")), ULL2NUM(access.response_body_bytes));
    hash_set_string(event, "remote_addr", access.remote_address);
    hash_set_string(event, "protocol", access.protocol);
    hash_set_string(event, "host", access.host);
    hash_set_string(event, "user_agent", access.user_agent);
    hash_set_string(event, "referer", access.referer);
    hash_set_string(event, "request_id", access.request_id);
    rb_hash_aset(event, ID2SYM(rb_intern("worker_pid")), INT2NUM(access.worker_pid));
    rb_hash_aset(event, ID2SYM(rb_intern("worker_index")), INT2NUM(access.worker_index));
    hash_set_string(event, "connection_outcome", access.connection_outcome);
    hash_set_string(event, "trace_id", access.trace_id);
    hash_set_string(event, "span_id", access.span_id);
    hash_set_string(event, "outcome", queued.outcome);
    hash_set_string(event, "failure_kind", queued.failure_kind);
    rb_hash_aset(event, ID2SYM(rb_intern("response_sent")), queued.response_sent ? Qtrue : Qfalse);
    hash_set_string(event, "error_message", queued.error_message);
    hash_set_string(event, "timestamp", queued.timestamp);
    return event;
  }

  VALUE request_observability_event_to_span_array(const Vajra::runtime::RequestSpanEvent &event)
  {
    VALUE payload = rb_ary_new_capa(15);
    rb_ary_push(payload, rb_str_new(event.method.c_str(), static_cast<long>(event.method.size())));
    rb_ary_push(payload, rb_str_new(event.target.c_str(), static_cast<long>(event.target.size())));
    rb_ary_push(payload, INT2NUM(event.status_code));
    rb_ary_push(payload, LL2NUM(event.duration_nanoseconds));
    rb_ary_push(payload, rb_str_new(event.protocol.c_str(), static_cast<long>(event.protocol.size())));
    rb_ary_push(payload, rb_str_new(event.host.c_str(), static_cast<long>(event.host.size())));
    rb_ary_push(payload, rb_str_new(event.outcome.c_str(), static_cast<long>(event.outcome.size())));
    rb_ary_push(payload, rb_str_new(event.failure_kind.c_str(), static_cast<long>(event.failure_kind.size())));
    rb_ary_push(payload, event.response_sent ? Qtrue : Qfalse);
    rb_ary_push(payload, rb_str_new(event.connection_outcome.c_str(), static_cast<long>(event.connection_outcome.size())));
    rb_ary_push(payload, INT2NUM(event.worker_index));
    rb_ary_push(payload, INT2NUM(event.worker_pid));
    rb_ary_push(payload, rb_str_new(event.trace_id.c_str(), static_cast<long>(event.trace_id.size())));
    rb_ary_push(payload, rb_str_new(event.span_id.c_str(), static_cast<long>(event.span_id.size())));
    rb_ary_push(payload, rb_str_new(event.error_message.c_str(), static_cast<long>(event.error_message.size())));
    return payload;
  }

  VALUE rb_tracing_native_drain_request_observability_events(VALUE self, VALUE limit)
  {
    (void)self;
    const auto events = Vajra::runtime::drain_runtime_request_observability_events(NUM2SIZET(limit));
    VALUE list = rb_ary_new_capa(static_cast<long>(events.size()));
    for (const auto &event : events)
    {
      rb_ary_push(list, request_observability_event_to_hash(event));
    }
    return list;
  }

  VALUE rb_tracing_native_drain_request_span_events(VALUE self, VALUE limit)
  {
    (void)self;
    const auto events = Vajra::runtime::drain_runtime_request_span_events(NUM2SIZET(limit));
    VALUE list = rb_ary_new_capa(static_cast<long>(events.size()));
    for (const auto &event : events)
    {
      rb_ary_push(list, request_observability_event_to_span_array(event));
    }
    return list;
  }

  VALUE rb_tracing_native_set_status(
      VALUE self,
      VALUE enabled,
      VALUE available,
      VALUE endpoint,
      VALUE service_name,
      VALUE active_context_required,
      VALUE sample_ratio)
  {
    (void)self;
    Vajra::runtime::configure_runtime_tracing(
        enabled == Qtrue,
        NIL_P(endpoint) ? std::string() : StringValueCStr(endpoint),
        NIL_P(service_name) ? std::string("vajra") : StringValueCStr(service_name),
        active_context_required == Qtrue);
    Vajra::runtime::set_runtime_trace_sample_ratio(NUM2DBL(sample_ratio));
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
      "__native_set_request_observability_callback__",
      RUBY_METHOD_FUNC(rb_tracing_native_set_request_observability_callback),
      1);
  rb_define_singleton_method(
      mTracing,
      "__native_drain_request_observability_events__",
      RUBY_METHOD_FUNC(rb_tracing_native_drain_request_observability_events),
      1);
  rb_define_singleton_method(
      mTracing,
      "__native_drain_request_span_events__",
      RUBY_METHOD_FUNC(rb_tracing_native_drain_request_span_events),
      1);
  rb_define_singleton_method(
      mTracing,
      "__native_set_tracing_status__",
      RUBY_METHOD_FUNC(rb_tracing_native_set_status),
      6);
}
