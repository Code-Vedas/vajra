# frozen_string_literal: true

# Copyright Codevedas Inc. 2025-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

module Vajra
  module Internal
    # Soft-optional OpenTelemetry bridge for request and runtime lifecycle spans.
    module Tracing
      module_function

      # Stores the current tracing runtime state shared by the Ruby bridge.
      TraceState = Struct.new(:enabled, :available, :tracer, :warning_emitted, keyword_init: true)
      TRACE_MUTEX = Mutex.new
      TRACE_STATE = TraceState.new(enabled: false, available: false, tracer: nil, warning_emitted: false)

      def install_from_start_options!(options)
        trace_enabled = options.fetch(:trace_enabled, false)
        trace_endpoint = options.fetch(:trace_endpoint, '').to_s
        trace_service_name = options.fetch(:trace_service_name, 'vajra').to_s

        unless trace_enabled
          update_native_status(enabled: false, available: false, endpoint: trace_endpoint, service_name: trace_service_name)
          install_lifecycle_callback(nil)
          write_trace_state(enabled: false, available: false, tracer: nil)
          return false
        end

        tracer = build_tracer(trace_endpoint, trace_service_name)
        available = !tracer.equal?(nil)

        write_trace_state(enabled: true, available:, tracer:)

        update_native_status(
          enabled: true,
          available: available,
          endpoint: trace_endpoint,
          service_name: trace_service_name
        )
        install_lifecycle_callback(available ? method(:emit_lifecycle_span) : nil)
        available
      end

      def with_request_span(env, &)
        tracer = TRACE_MUTEX.synchronize { TRACE_STATE.tracer }
        case tracer
        when nil
          yield
        else
          method = env['REQUEST_METHOD']
          path = env['PATH_INFO']
          tracer.in_span('vajra.request', attributes: compact_attributes(
            'vajra.request.method' => method,
            'vajra.request.path' => path
          ), &)
        end
      end

      def emit_lifecycle_span(event)
        tracer = TRACE_MUTEX.synchronize { TRACE_STATE.tracer }
        return unless tracer

        event_name = event.fetch(:event)
        tracer.in_span("vajra.#{event_name}", attributes: compact_attributes(
          'vajra.worker.index' => event[:worker_index],
          'vajra.worker.pid' => event[:pid],
          'vajra.worker.lifecycle_state' => event[:lifecycle_state],
          'vajra.worker.health_state' => event[:health_state],
          'vajra.worker.recovery_state' => event[:recovery_state],
          'vajra.worker.available' => event[:available],
          'vajra.worker.exit_classification' => event[:exit_classification],
          'vajra.worker.replacement_needed' => event[:replacement_needed],
          'vajra.worker.terminal_replacement_failure' => event[:terminal_replacement_failure],
          'vajra.worker.exit_detail' => event[:exit_detail]
        )) { |_span| nil }
      end

      def compact_attributes(attributes)
        attributes.compact
      end
      private_class_method :compact_attributes

      def write_trace_state(enabled:, available:, tracer:)
        TRACE_MUTEX.synchronize do
          TRACE_STATE.enabled = enabled
          TRACE_STATE.available = available
          TRACE_STATE.tracer = tracer
        end
      end
      private_class_method :write_trace_state

      def build_tracer(trace_endpoint, trace_service_name)
        require 'opentelemetry/sdk'
        tracer_provider = OpenTelemetry::SDK::Trace::TracerProvider.new

        begin
          unless trace_endpoint.empty?
            require 'opentelemetry/exporter/otlp'
            exporter = OpenTelemetry::Exporter::OTLP::Exporter.new(endpoint: trace_endpoint)
            processor = OpenTelemetry::SDK::Trace::Export::SimpleSpanProcessor.new(exporter)
            tracer_provider.add_span_processor(processor)
          end
        rescue LoadError
          emit_missing_dependency_warning
          return nil
        end

        OpenTelemetry.tracer_provider = tracer_provider
        OpenTelemetry.tracer_provider.tracer(trace_service_name)
      rescue LoadError
        emit_missing_dependency_warning
        nil
      end
      private_class_method :build_tracer

      def emit_missing_dependency_warning
        TRACE_MUTEX.synchronize do
          return if TRACE_STATE.warning_emitted

          TRACE_STATE.warning_emitted = true
          warn('[Vajra][error] OpenTelemetry tracing requested, but required gems are not installed; tracing disabled')
        end
      end
      private_class_method :emit_missing_dependency_warning

      def update_native_status(config)
        __native_set_tracing_status__(config.fetch(:enabled), config.fetch(:available), config.fetch(:endpoint), config.fetch(:service_name))
      end
      private_class_method :update_native_status

      def install_lifecycle_callback(callback)
        __native_set_lifecycle_callback__(callback)
      end
      private_class_method :install_lifecycle_callback
    end
  end
end
