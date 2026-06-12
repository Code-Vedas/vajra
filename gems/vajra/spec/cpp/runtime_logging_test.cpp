// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "runtime/runtime_logging.hpp"
#include "runtime/traceparent.hpp"
#include "test_support.hpp"
#include "test_suites.hpp"

namespace
{
  void expect_needs(
      const Vajra::runtime::AccessLogFieldNeeds &needs,
      bool host,
      bool user_agent,
      bool referer,
      bool request_id,
      bool trace_context,
      const char *message)
  {
    if (needs.host != host ||
        needs.user_agent != user_agent ||
        needs.referer != referer ||
        needs.request_id != request_id ||
        needs.trace_context != trace_context)
    {
      VajraSpecCpp::fail(message);
    }
  }

  void configure_access_format(const std::string &format, bool structured_logs = false)
  {
    Vajra::runtime::configure_runtime_logging(structured_logs, "/dev/null", "", format);
    Vajra::runtime::configure_runtime_tracing(false, "", "");
    Vajra::runtime::set_runtime_tracing_available(false);
  }

  void test_common_access_log_needs_no_request_headers()
  {
    configure_access_format("common");

    expect_needs(
        Vajra::runtime::access_log_field_needs(),
        false,
        false,
        false,
        false,
        false,
        "common access log format should not request optional headers");
  }

  void test_combined_access_log_needs_referer_and_user_agent()
  {
    configure_access_format("combined");

    expect_needs(
        Vajra::runtime::access_log_field_needs(),
        false,
        true,
        true,
        false,
        false,
        "combined access log format should request referer and user-agent only");
  }

  void test_json_access_log_needs_structured_fields()
  {
    configure_access_format("json");

    expect_needs(
        Vajra::runtime::access_log_field_needs(),
        true,
        true,
        true,
        true,
        true,
        "json access log format should request all structured fields");
  }

  void test_custom_access_log_needs_token_fields()
  {
    configure_access_format("%m %h %i %T");

    expect_needs(
        Vajra::runtime::access_log_field_needs(),
        true,
        false,
        false,
        true,
        true,
        "custom access log format should request only token fields");
  }

  void test_tracing_forces_trace_context_need()
  {
    configure_access_format("common");
    Vajra::runtime::configure_runtime_tracing(true, "http://127.0.0.1:4318/v1/traces", "vajra-test");
    Vajra::runtime::set_runtime_tracing_available(true);

    expect_needs(
        Vajra::runtime::access_log_field_needs(),
        false,
        false,
        false,
        false,
        true,
        "available tracing should request trace context even when access format does not");
  }

  void test_active_context_required_follows_availability()
  {
    Vajra::runtime::configure_runtime_tracing(true, "http://127.0.0.1:4318/v1/traces", "vajra-test", true);
    Vajra::runtime::set_runtime_tracing_available(false);
    if (Vajra::runtime::runtime_tracing_active_context_required())
    {
      VajraSpecCpp::fail("unavailable tracing should not require active Rack context");
    }

    Vajra::runtime::set_runtime_tracing_available(true);
    if (!Vajra::runtime::runtime_tracing_active_context_required())
    {
      VajraSpecCpp::fail("available app-owned tracing should require active Rack context");
    }

    Vajra::runtime::configure_runtime_tracing(true, "http://127.0.0.1:4318/v1/traces", "vajra-test", false);
    Vajra::runtime::set_runtime_tracing_available(true);
    if (Vajra::runtime::runtime_tracing_active_context_required())
    {
      VajraSpecCpp::fail("Vajra-owned tracing should use native queued spans without active Rack context");
    }
  }

  void test_runtime_trace_sampling_uses_ratio_and_parent_flags()
  {
    Vajra::runtime::set_runtime_request_observability_callback(reinterpret_cast<void *>(1));
    Vajra::runtime::configure_runtime_tracing(
        true,
        "http://127.0.0.1:4318/v1/traces",
        "vajra-test",
        false,
        "service.namespace=vajra.test",
        "tracecontext,baggage");
    if (Vajra::runtime::runtime_tracing_resource_attributes() != "service.namespace=vajra.test")
    {
      VajraSpecCpp::fail("runtime tracing should retain OTEL resource attributes for native export");
    }
    Vajra::runtime::set_runtime_trace_sample_ratio(0.0);
    if (Vajra::runtime::runtime_trace_sampled(""))
    {
      VajraSpecCpp::fail("zero trace sample ratio should skip root request span events");
    }
    if (!Vajra::runtime::runtime_trace_sampled("00-11111111111111111111111111111111-2222222222222222-01"))
    {
      VajraSpecCpp::fail("sampled traceparent should preserve parent sampling");
    }
    if (Vajra::runtime::runtime_trace_sampled("00-11111111111111111111111111111111-2222222222222222-00"))
    {
      VajraSpecCpp::fail("unsampled traceparent should preserve parent sampling");
    }

    Vajra::runtime::configure_runtime_tracing(
        true,
        "http://127.0.0.1:4318/v1/traces",
        "vajra-test",
        false,
        "",
        "none");
    if (Vajra::runtime::runtime_tracecontext_propagator_enabled())
    {
      VajraSpecCpp::fail("OTEL_PROPAGATORS=none should disable tracecontext extraction");
    }
    if (Vajra::runtime::runtime_trace_sampled("00-11111111111111111111111111111111-2222222222222222-01"))
    {
      VajraSpecCpp::fail("disabled tracecontext propagator should ignore parent sampling");
    }

    Vajra::runtime::set_runtime_trace_sample_ratio(1.0);
    if (!Vajra::runtime::runtime_trace_sampled(""))
    {
      VajraSpecCpp::fail("full trace sample ratio should sample root request span events");
    }
    Vajra::runtime::set_runtime_request_observability_callback(nullptr);
    if (Vajra::runtime::runtime_trace_sampled(""))
    {
      VajraSpecCpp::fail("disabled request observability should skip sampling");
    }
  }

  void test_traceparent_part_allows_future_version_fields()
  {
    const std::string traceparent =
        "01-11111111111111111111111111111111-2222222222222222-01-future-field";

    if (Vajra::runtime::traceparent_part(traceparent, 1) != "11111111111111111111111111111111" ||
        Vajra::runtime::traceparent_part(traceparent, 2) != "2222222222222222" ||
        Vajra::runtime::traceparent_part(traceparent, 3) != "01")
    {
      VajraSpecCpp::fail("future traceparent version should allow extra delimited fields");
    }
    if (!Vajra::runtime::traceparent_part(
             "00-11111111111111111111111111111111-2222222222222222-01-extra",
             1)
             .empty())
    {
      VajraSpecCpp::fail("traceparent version 00 should reject extra fields");
    }
    if (!Vajra::runtime::traceparent_part(
             "01-11111111111111111111111111111111-2222222222222222-01extra",
             1)
             .empty())
    {
      VajraSpecCpp::fail("future traceparent flags should be followed by a delimiter when extra fields exist");
    }
    if (!Vajra::runtime::traceparent_part(
             "ff-11111111111111111111111111111111-2222222222222222-01",
             1)
             .empty())
    {
      VajraSpecCpp::fail("traceparent version ff should be rejected");
    }
  }

  Vajra::runtime::AccessLogEvent sample_access_event(int status)
  {
    Vajra::runtime::AccessLogEvent event;
    event.method = "GET";
    event.target = "/";
    event.status_code = status;
    event.duration_nanoseconds = 1000;
    event.response_body_bytes = 2;
    event.remote_address = "127.0.0.1";
    event.protocol = "HTTP/1.1";
    event.connection_outcome = "close";
    return event;
  }

  void test_async_logger_reuses_pooled_nodes()
  {
    configure_access_format("common");
    Vajra::runtime::start_runtime_logging_worker();
    const std::size_t initial_blocks = Vajra::runtime::runtime_logging_node_block_count();
    for (int index = 0; index < 32; ++index)
    {
      Vajra::runtime::log_access_event(sample_access_event(200));
    }
    Vajra::runtime::flush_runtime_logs();
    const std::size_t after_first_batch = Vajra::runtime::runtime_logging_node_block_count();
    for (int index = 0; index < 32; ++index)
    {
      Vajra::runtime::log_access_event(sample_access_event(204));
    }
    Vajra::runtime::flush_runtime_logs();
    const std::size_t after_second_batch = Vajra::runtime::runtime_logging_node_block_count();
    Vajra::runtime::stop_runtime_logging_worker();

    if (initial_blocks == 0 || after_first_batch != initial_blocks || after_second_batch != initial_blocks)
    {
      VajraSpecCpp::fail("async logger did not reuse pooled queue nodes across batches");
    }
  }

  void test_request_observability_queue_drains_enabled_events()
  {
    const std::uint64_t initial_events = Vajra::runtime::runtime_native_request_observability_events_total();
    const std::uint64_t initial_errors = Vajra::runtime::runtime_native_request_observability_errors_total();
    const std::uint64_t initial_admissions = Vajra::runtime::runtime_native_request_admission_rejections_total();
    Vajra::runtime::set_runtime_request_observability_callback(reinterpret_cast<void *>(1));

    Vajra::runtime::emit_runtime_request_observability_event(
        sample_access_event(503),
        "queue_capacity",
        "queue_capacity",
        true,
        "full");
    Vajra::runtime::emit_runtime_request_observability_event(
        sample_access_event(204),
        "completed",
        "",
        true,
        "");

    std::vector<Vajra::runtime::RequestObservabilityEvent> first_batch =
        Vajra::runtime::drain_runtime_request_observability_events(1);
    std::vector<Vajra::runtime::RequestObservabilityEvent> second_batch =
        Vajra::runtime::drain_runtime_request_observability_events(8);
    Vajra::runtime::set_runtime_request_observability_callback(nullptr);

    if (first_batch.size() != 1 || second_batch.size() != 1)
    {
      VajraSpecCpp::fail("request observability queue did not drain in requested batch sizes");
    }
    if (first_batch[0].outcome != "queue_capacity" || second_batch[0].outcome != "completed")
    {
      VajraSpecCpp::fail("request observability queue did not preserve event order");
    }
    if (Vajra::runtime::runtime_native_request_observability_events_total() != initial_events + 2 ||
        Vajra::runtime::runtime_native_request_observability_errors_total() != initial_errors + 1 ||
        Vajra::runtime::runtime_native_request_admission_rejections_total() != initial_admissions + 1)
    {
      VajraSpecCpp::fail("request observability native counters did not accumulate expected totals");
    }
  }

  void test_request_observability_disabled_queue_keeps_counters()
  {
    const std::uint64_t initial_events = Vajra::runtime::runtime_native_request_observability_events_total();
    Vajra::runtime::set_runtime_request_observability_callback(nullptr);

    Vajra::runtime::emit_runtime_request_observability_event(
        sample_access_event(500),
        "execution_error",
        "execution_error",
        true,
        "boom");

    std::vector<Vajra::runtime::RequestObservabilityEvent> drained =
        Vajra::runtime::drain_runtime_request_observability_events(8);
    if (!drained.empty())
    {
      VajraSpecCpp::fail("disabled request observability should not queue events");
    }
    if (Vajra::runtime::runtime_native_request_observability_events_total() != initial_events + 1)
    {
      VajraSpecCpp::fail("disabled request observability should still update native counters");
    }
  }

  void test_request_span_events_drain_to_ruby_observability_queue()
  {
    Vajra::runtime::set_runtime_request_observability_callback(reinterpret_cast<void *>(1));

    Vajra::runtime::RequestSpanEvent event;
    event.method = "GET";
    event.target = "/direct";
    event.status_code = 204;
    event.duration_nanoseconds = 1000;
    event.protocol = "HTTP/1.1";
    event.host = "example.test";
    event.outcome = "completed";
    event.response_sent = true;
    event.connection_outcome = "keepalive";
    event.worker_index = 2;
    event.worker_pid = 1234;
    event.trace_id = "11111111111111111111111111111111";
    event.span_id = "2222222222222222";

    Vajra::runtime::emit_runtime_request_span_event(event);
    std::vector<Vajra::runtime::RequestObservabilityEvent> drained =
        Vajra::runtime::drain_runtime_request_observability_events(8);
    Vajra::runtime::set_runtime_request_observability_callback(nullptr);

    if (drained.size() != 1)
    {
      VajraSpecCpp::fail("direct request span event should drain to Ruby observability queue");
    }
    if (drained[0].access.method != "GET" ||
        drained[0].access.target != "/direct" ||
        drained[0].access.status_code != 204 ||
        drained[0].access.host != "example.test" ||
        drained[0].access.trace_id != "11111111111111111111111111111111" ||
        drained[0].outcome != "completed" ||
        !drained[0].response_sent)
    {
      VajraSpecCpp::fail("direct request span event did not preserve Ruby observability fields");
    }
  }
}

void VajraSpecCpp::run_runtime_logging_tests()
{
  test_common_access_log_needs_no_request_headers();
  test_combined_access_log_needs_referer_and_user_agent();
  test_json_access_log_needs_structured_fields();
  test_custom_access_log_needs_token_fields();
  test_tracing_forces_trace_context_need();
  test_active_context_required_follows_availability();
  test_runtime_trace_sampling_uses_ratio_and_parent_flags();
  test_traceparent_part_allows_future_version_fields();
  test_async_logger_reuses_pooled_nodes();
  test_request_observability_queue_drains_enabled_events();
  test_request_observability_disabled_queue_keeps_counters();
  test_request_span_events_drain_to_ruby_observability_queue();
}
