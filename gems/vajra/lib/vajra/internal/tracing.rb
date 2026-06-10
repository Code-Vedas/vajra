# frozen_string_literal: true

# Copyright Codevedas Inc. 2025-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

module Vajra
  module Internal
    # Soft-optional OpenTelemetry bridge for request and runtime lifecycle spans.
    # rubocop:disable Metrics/ModuleLength
    module Tracing
      module_function

      # Resolved tracing configuration from explicit options and env vars.
      TraceConfig = Struct.new(
        :enabled,
        :endpoint,
        :service_name,
        :otel_owner,
        :traces_exporter,
        :metrics_exporter,
        :propagators,
        :resource_attributes,
        :sampler,
        :sampler_arg,
        keyword_init: true
      )
      # Mutable process-local telemetry state shared by request and lifecycle hooks.
      TraceState = Struct.new(
        :enabled,
        :available,
        :tracer,
        :meter,
        :provider,
        :warning_emitted,
        :request_counter,
        :request_duration,
        :active_requests,
        :metric_instruments,
        :request_observability_thread,
        :request_observability_stop,
        keyword_init: true
      )
      TRACE_MUTEX = Mutex.new
      TRACE_STATE = TraceState.new(
        enabled: false,
        available: false,
        tracer: nil,
        meter: nil,
        provider: nil,
        warning_emitted: false,
        request_counter: nil,
        request_duration: nil,
        active_requests: nil,
        metric_instruments: {},
        request_observability_thread: nil,
        request_observability_stop: false
      )
      INTERNAL_TRACE_ID_HEADER = 'X-Vajra-Internal-Trace-Id'
      INTERNAL_SPAN_ID_HEADER = 'X-Vajra-Internal-Span-Id'
      REQUEST_OBSERVABILITY_BATCH_SIZE = 4096
      REQUEST_OBSERVABILITY_POLL_SECONDS = 0.01
      REQUEST_OBSERVABILITY_YIELD_SECONDS = 0.001

      def install_from_start_options!(options)
        config = resolve_config(options)
        endpoint = config.endpoint
        service_name = config.service_name

        unless config.enabled
          stop_request_observability_drain_thread
          update_native_status(enabled: false, available: false, endpoint:, service_name:)
          install_lifecycle_callback(nil)
          install_request_observability_callback(nil)
          write_trace_state(enabled: false, available: false, tracer: nil, meter: nil, provider: nil)
          return false
        end

        telemetry = build_telemetry(config)
        tracer = telemetry.fetch(:tracer)
        meter = telemetry.fetch(:meter)
        available = telemetry_available?(telemetry)
        otel_owner = config.otel_owner

        write_trace_state(
          enabled: true,
          available:,
          tracer:,
          meter:,
          provider: otel_owner ? telemetry.fetch(:provider) : nil
        )
        install_metric_instruments

        update_native_status(
          enabled: true,
          available: available,
          endpoint:,
          service_name:,
          active_context_required: available && !otel_owner,
          sample_ratio: native_sample_ratio(config)
        )
        install_telemetry_callbacks(telemetry)
        available
      end

      def install_telemetry_callbacks(telemetry)
        install_lifecycle_callback(ruby_callback_available?(telemetry) ? method(:emit_lifecycle_span) : nil)
        install_request_observability_callback(
          request_callback_available?(telemetry) ? method(:emit_native_request_span) : nil
        )
      end
      private_class_method :install_telemetry_callbacks

      def telemetry_available?(telemetry)
        !telemetry.fetch(:tracer).equal?(nil) || telemetry.fetch(:native_tracing, false)
      end
      private_class_method :telemetry_available?

      def ruby_callback_available?(telemetry)
        !telemetry.fetch(:tracer).equal?(nil) || !telemetry.fetch(:meter).equal?(nil)
      end
      private_class_method :ruby_callback_available?

      def request_callback_available?(telemetry)
        ruby_callback_available?(telemetry)
      end
      private_class_method :request_callback_available?

      def shutdown!
        provider = TRACE_MUTEX.synchronize { TRACE_STATE.provider }
        return unless provider

        provider.force_flush if provider.respond_to?(:force_flush)
        provider.shutdown if provider.respond_to?(:shutdown)
      ensure
        stop_request_observability_drain_thread
        install_lifecycle_callback(nil)
        install_request_observability_callback(nil)
        write_trace_state(enabled: false, available: false, tracer: nil, meter: nil, provider: nil)
      end

      def after_fork!
        enabled, output_available = TRACE_MUTEX.synchronize do
          [
            TRACE_STATE.enabled,
            TRACE_STATE.tracer || TRACE_STATE.meter
          ]
        end
        start_request_observability_drain_thread if enabled && output_available
      end

      def with_request_span(env, &)
        drain_request_observability_batch
        tracer = TRACE_MUTEX.synchronize { TRACE_STATE.tracer }
        tracer_missing = tracer.equal?(nil)
        return yield if tracer_missing && !request_metrics_available?
        return with_request_metrics_only(env, &) if tracer_missing
        return with_request_metrics_or_direct(env, &) unless span_attemptable?(tracer, env)

        with_traced_request_span(env, tracer, &)
      end

      def with_traced_request_span(env, tracer)
        attributes = request_attributes(env)
        span_name = request_span_name(env)
        context = extract_context(env)
        options = { attributes: attributes }

        record_active_request(1, attributes)
        started_at = monotonic_seconds
        response_status = nil
        begin
          in_span(tracer, span_name, options, context) do |span|
            result = yield
            response_status = response_status_from(result)
            record_span_response_status(span, response_status)
            mark_span_response_outcome(span, response_status)
            attach_trace_context(result, span)
          rescue StandardError => e
            record_span_exception(span, e)
            raise
          end
        ensure
          duration = monotonic_seconds - started_at
          metric_attributes = response_status ? attributes.merge('http.response.status_code' => response_status) : attributes
          record_request_metrics(duration, metric_attributes)
          record_active_request(-1, attributes)
        end
      end
      private_class_method :with_traced_request_span

      def emit_lifecycle_span(event)
        tracer = TRACE_MUTEX.synchronize { TRACE_STATE.tracer }

        event_name = event.fetch(:event)
        metric_attributes = lifecycle_attributes(event)
        record_named_metric(:worker_lifecycle_counter, 1, metric_attributes)
        record_named_metric(:worker_health_counter, 1, metric_attributes)
        return unless tracer

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

      def emit_native_request_span(event)
        tracer = TRACE_MUTEX.synchronize { TRACE_STATE.tracer }
        return record_native_request_metrics_only(event) unless span_attemptable?(tracer, event)

        attributes = native_request_attributes(event)
        duration = nanoseconds_to_seconds(event[:duration_nanoseconds])

        options = { attributes: attributes }
        context = native_parent_context(event)
        in_span(tracer, native_request_span_name(event), options, context) do |span|
          status = integer_or_nil(event[:status])
          record_span_response_status(span, status)
          mark_span_success(span) if status && status < 500 && event[:response_sent]
          record_span_error(span, event[:error_message].to_s) unless native_success_event?(event)
        end
        record_request_metrics(duration, attributes)
        record_native_request_metrics(event, attributes)
      end

      def resolve_config(options)
        otel_traces_exporter = env_value('OTEL_TRACES_EXPORTER')
        TraceConfig.new(
          enabled: resolve_boolean(options, :trace_enabled, 'VAJRA_TRACE_ENABLED') do
            !otel_traces_exporter.nil? && otel_traces_exporter != 'none'
          end,
          endpoint: resolve_string(options, :trace_endpoint, 'VAJRA_TRACE_ENDPOINT', env_value('OTEL_EXPORTER_OTLP_ENDPOINT') || ''),
          service_name: resolve_string(options, :trace_service_name, 'VAJRA_TRACE_SERVICE_NAME', env_value('OTEL_SERVICE_NAME') || 'vajra'),
          otel_owner: resolve_boolean(options, :trace_otel_owner, 'VAJRA_TRACE_OTEL_OWNER') { false },
          traces_exporter: otel_traces_exporter || 'otlp',
          metrics_exporter: env_value('OTEL_METRICS_EXPORTER') || 'none',
          propagators: env_value('OTEL_PROPAGATORS') || 'tracecontext,baggage',
          resource_attributes: env_value('OTEL_RESOURCE_ATTRIBUTES') || '',
          sampler: env_value('OTEL_TRACES_SAMPLER') || '',
          sampler_arg: env_value('OTEL_TRACES_SAMPLER_ARG') || ''
        )
      end
      private_class_method :resolve_config

      def resolve_string(options, key, env_name, default_value)
        return options.fetch(key).to_s if options.key?(key)

        env_value(env_name) || default_value
      end
      private_class_method :resolve_string

      def resolve_boolean(options, key, env_name)
        return boolean_value(options.fetch(key), key.to_s) if options.key?(key)

        value = env_value(env_name)
        return boolean_value(value, env_name) unless value.nil?

        yield
      end
      private_class_method :resolve_boolean

      def boolean_value(value, name)
        case value
        when true, 'true', '1', 'yes', 'on', 'owner', 'vajra'
          true
        when false, 'false', '0', 'no', 'off', 'none'
          false
        else
          raise ArgumentError, "invalid #{name}: #{value.inspect}"
        end
      end
      private_class_method :boolean_value

      def env_value(name)
        value = ENV.fetch(name, nil)
        return nil if value.nil?

        trimmed = value.strip
        trimmed.empty? ? nil : trimmed
      end
      private_class_method :env_value

      def build_telemetry(config)
        return native_owned_telemetry(config) if config.otel_owner

        require 'opentelemetry/sdk'
        provider = configured_tracer_provider(config) if tracer_provider_enabled?(config)
        tracer = provider&.tracer(config.service_name)
        meter = configured_meter(config)
        { provider:, tracer:, meter:, native_tracing: false }
      rescue LoadError
        emit_missing_dependency_warning
        { provider: nil, tracer: nil, meter: nil, native_tracing: false }
      end
      private_class_method :build_telemetry

      def native_owned_telemetry(config)
        {
          provider: nil,
          tracer: nil,
          meter: nil,
          native_tracing: native_tracing_configured?(config)
        }
      end
      private_class_method :native_owned_telemetry

      def native_tracing_configured?(config)
        return false if sampler_always_off?(config)
        return false if config.traces_exporter == 'none'

        config.endpoint.start_with?('http://')
      end
      private_class_method :native_tracing_configured?

      def sampler_always_off?(config)
        config.sampler.to_s == 'always_off'
      end
      private_class_method :sampler_always_off?

      def tracer_provider_enabled?(config)
        !sampler_always_off?(config) && !span_export_disabled?(config)
      end
      private_class_method :tracer_provider_enabled?

      def native_sample_ratio(config)
        case config.sampler.to_s
        when 'always_off', 'parentbased_always_off'
          0.0
        when 'traceidratio', 'parentbased_traceidratio'
          bounded_sample_ratio(config.sampler_arg)
        else
          1.0
        end
      end
      private_class_method :native_sample_ratio

      def bounded_sample_ratio(raw)
        ratio = Float(raw)
        return 0.0 if ratio.negative?
        return 1.0 if ratio > 1.0

        ratio
      rescue ArgumentError, TypeError
        1.0
      end
      private_class_method :bounded_sample_ratio

      def span_export_disabled?(config)
        config.otel_owner && config.traces_exporter == 'none' && config.endpoint.empty?
      end
      private_class_method :span_export_disabled?

      def configured_tracer_provider(_config)
        existing_provider = OpenTelemetry.tracer_provider if OpenTelemetry.respond_to?(:tracer_provider)
        existing_provider
      end
      private_class_method :configured_tracer_provider

      def configured_meter(config)
        return nil if config.metrics_exporter == 'none'
        return nil unless OpenTelemetry.respond_to?(:meter_provider)

        meter_provider = OpenTelemetry.meter_provider
        return nil if meter_provider.nil?

        meter_provider.meter(config.service_name)
      end
      private_class_method :configured_meter

      def install_metric_instruments
        meter = TRACE_MUTEX.synchronize { TRACE_STATE.meter }
        return unless meter

        request_counter = build_instrument(meter, :create_counter, 'vajra.http.server.requests')
        request_duration = build_instrument(meter, :create_histogram, 'vajra.http.server.duration')
        active_requests = build_instrument(meter, :create_up_down_counter, 'vajra.http.server.active_requests')
        metric_instruments = {
          native_request_counter: build_instrument(meter, :create_counter, 'vajra.native.request.outcomes'),
          native_error_counter: build_instrument(meter, :create_counter, 'vajra.native.request.errors'),
          admission_counter: build_instrument(meter, :create_counter, 'vajra.request.admission.outcomes'),
          worker_lifecycle_counter: build_instrument(meter, :create_counter, 'vajra.worker.lifecycle.events'),
          worker_health_counter: build_instrument(meter, :create_counter, 'vajra.worker.health.events'),
          worker_restart_counter: build_instrument(meter, :create_counter, 'vajra.worker.restarts'),
          accept_counter: build_instrument(meter, :create_counter, 'vajra.worker.accepts'),
          runtime_error_counter: build_instrument(meter, :create_counter, 'vajra.runtime.errors'),
          active_executions: build_instrument(meter, :create_up_down_counter, 'vajra.worker.active_executions'),
          idle_executions: build_instrument(meter, :create_up_down_counter, 'vajra.worker.idle_executions')
        }
        TRACE_MUTEX.synchronize do
          TRACE_STATE.request_counter = request_counter
          TRACE_STATE.request_duration = request_duration
          TRACE_STATE.active_requests = active_requests
          TRACE_STATE.metric_instruments = metric_instruments
        end
      end
      private_class_method :install_metric_instruments

      def build_instrument(meter, factory, name)
        return nil unless meter.respond_to?(factory)

        meter.public_send(factory, name, unit: factory == :create_histogram ? 's' : '1')
      rescue ArgumentError
        meter.public_send(factory, name)
      end
      private_class_method :build_instrument

      def request_attributes(env)
        protocol_name, protocol_version = protocol_attributes(env)
        compact_attributes(
          'http.request.method' => env['REQUEST_METHOD'],
          'url.path' => env['PATH_INFO'],
          'url.scheme' => env['rack.url_scheme'],
          'http.response.status_code' => nil,
          'server.address' => env['SERVER_NAME'],
          'server.port' => integer_or_nil(env['SERVER_PORT']),
          'network.protocol.name' => protocol_name,
          'network.protocol.version' => protocol_version
        )
      end
      private_class_method :request_attributes

      def protocol_attributes(env)
        protocol = env['SERVER_PROTOCOL'].to_s
        return ['http', protocol.split('/').last] if protocol.start_with?('HTTP/')

        ['http', nil]
      end
      private_class_method :protocol_attributes

      def request_span_name(env)
        method = env['REQUEST_METHOD'].to_s
        method.empty? ? 'HTTP request' : method
      end
      private_class_method :request_span_name

      def extract_context(env)
        return nil unless defined?(OpenTelemetry) && OpenTelemetry.respond_to?(:propagation)

        carrier = {
          'traceparent' => env['HTTP_TRACEPARENT'],
          'tracestate' => env['HTTP_TRACESTATE']
        }.compact
        return nil if carrier.empty?

        OpenTelemetry.propagation.extract(carrier)
      rescue NoMethodError
        nil
      end
      private_class_method :extract_context

      def in_span(tracer, name, options, parent_context, &)
        return tracer.in_span(name, **options, &) unless parent_context && tracer.respond_to?(:start_span) && defined?(OpenTelemetry::Trace)

        span = tracer.start_span(name, **options, with_parent: parent_context)
        OpenTelemetry::Trace.with_span(span) { |active_span, _context| yield active_span }
      rescue Exception => e # rubocop:disable Lint/RescueException
        span.record_exception(e) if !span.nil? && span.respond_to?(:record_exception)
        record_span_error(span, "Unhandled exception of type: #{e.class}") if span
        raise
      ensure
        span.finish if !span.nil? && span.respond_to?(:finish)
      end
      private_class_method :in_span

      def with_request_metrics_only(env)
        attributes = request_attributes(env)
        record_active_request(1, attributes)
        started_at = monotonic_seconds
        response_status = nil
        begin
          yield.tap do |result|
            response_status = response_status_from(result)
          end
        ensure
          duration = monotonic_seconds - started_at
          metric_attributes = response_status ? attributes.merge('http.response.status_code' => response_status) : attributes
          record_request_metrics(duration, metric_attributes)
          record_active_request(-1, attributes)
        end
      end
      private_class_method :with_request_metrics_only

      def with_request_metrics_or_direct(env, &)
        return yield unless request_metrics_available?

        with_request_metrics_only(env, &)
      end
      private_class_method :with_request_metrics_or_direct

      def request_metrics_available?
        TRACE_MUTEX.synchronize do
          [TRACE_STATE.request_counter, TRACE_STATE.request_duration, TRACE_STATE.active_requests].any?
        end
      end
      private_class_method :request_metrics_available?

      def span_attemptable?(tracer, carrier)
        return false unless tracer
        return false if tracer.respond_to?(:enabled?) && !tracer.enabled?
        return false if tracer.respond_to?(:recording?) && !tracer.recording?
        return tracer.sampled?(carrier) if tracer.respond_to?(:sampled?)

        true
      rescue ArgumentError, NoMethodError
        true
      end
      private_class_method :span_attemptable?

      def mark_span_success(span)
        return unless span.respond_to?(:status=)

        span.status = OpenTelemetry::Trace::Status.ok if defined?(OpenTelemetry::Trace::Status)
      end
      private_class_method :mark_span_success

      def mark_span_response_outcome(span, status)
        return unless status

        status < 500 ? mark_span_success(span) : record_span_error(span, "HTTP #{status}")
      end
      private_class_method :mark_span_response_outcome

      def record_span_response_status(span, status)
        return unless span && status && span.respond_to?(:set_attribute)

        span.set_attribute('http.response.status_code', status)
      end
      private_class_method :record_span_response_status

      def response_status_from(result)
        return nil unless result.respond_to?(:[])

        Integer(result[0])
      rescue ArgumentError, TypeError
        nil
      end
      private_class_method :response_status_from

      def record_span_exception(span, error)
        return unless span

        span.record_exception(error) if span.respond_to?(:record_exception)
        record_span_error(span, error.message)
      end
      private_class_method :record_span_exception

      def record_span_error(span, message)
        span.status = OpenTelemetry::Trace::Status.error(message) if span.respond_to?(:status=) && defined?(OpenTelemetry::Trace::Status)
      end
      private_class_method :record_span_error

      def current_span
        OpenTelemetry::Trace.current_span if defined?(OpenTelemetry::Trace) && OpenTelemetry::Trace.respond_to?(:current_span)
      end
      private_class_method :current_span

      def current_trace_context(span = current_span)
        context = span_context_for(span)
        compact_attributes(
          trace_id: trace_context_value(context, :trace_id, :hex_trace_id),
          span_id: trace_context_value(context, :span_id, :hex_span_id)
        )
      end

      def span_context_for(span)
        return nil unless span

        return span.context if span.respond_to?(:context)
        return span.span_context if span.respond_to?(:span_context)

        nil
      end
      private_class_method :span_context_for

      def trace_context_value(context, primary, fallback)
        return nil unless context

        value = context.public_send(fallback) if context.respond_to?(fallback)
        return value.to_s unless value.nil?

        value = context.public_send(primary) if context.respond_to?(primary)
        value&.to_s
      end
      private_class_method :trace_context_value

      def attach_trace_context(result, span)
        trace_context = current_trace_context(span)
        return result if trace_context.empty? || !result.respond_to?(:[])

        headers = result[1]
        return result unless headers.respond_to?(:[]=) || headers.respond_to?(:<<)

        trace_id = trace_context[:trace_id]
        span_id = trace_context[:span_id]
        if headers.is_a?(Array)
          headers << [INTERNAL_TRACE_ID_HEADER, trace_id] if trace_id
          headers << [INTERNAL_SPAN_ID_HEADER, span_id] if span_id
        else
          headers[INTERNAL_TRACE_ID_HEADER] = trace_id if trace_id
          headers[INTERNAL_SPAN_ID_HEADER] = span_id if span_id
        end
        result
      end
      private_class_method :attach_trace_context

      def record_request_metrics(duration, attributes)
        request_counter, request_duration = TRACE_MUTEX.synchronize do
          [TRACE_STATE.request_counter, TRACE_STATE.request_duration]
        end
        request_counter&.add(1, attributes: attributes)
        request_duration&.record(duration, attributes: attributes)
      rescue ArgumentError
        request_counter&.add(1)
        request_duration&.record(duration)
      end
      private_class_method :record_request_metrics

      def record_native_request_metrics(event, attributes)
        outcome = event[:outcome].to_s
        record_named_metric(:native_request_counter, 1, attributes)
        record_named_metric(:native_error_counter, 1, attributes) unless native_success_event?(event)
        record_named_metric(:admission_counter, 1, attributes) if %w[queue_capacity request_timeout].include?(outcome)
      end
      private_class_method :record_native_request_metrics

      def record_native_request_metrics_only(event)
        attributes = native_request_attributes(event)
        duration = nanoseconds_to_seconds(event[:duration_nanoseconds])
        record_request_metrics(duration, attributes)
        record_native_request_metrics(event, attributes)
      end
      private_class_method :record_native_request_metrics_only

      def record_named_metric(name, value, attributes)
        instrument = TRACE_MUTEX.synchronize { TRACE_STATE.metric_instruments.fetch(name, nil) }
        instrument&.add(value, attributes: attributes)
      rescue ArgumentError
        instrument&.add(value)
      end
      private_class_method :record_named_metric

      def record_active_request(delta, attributes)
        active_requests = TRACE_MUTEX.synchronize { TRACE_STATE.active_requests }
        active_requests&.add(delta, attributes: attributes)
      rescue ArgumentError
        active_requests&.add(delta)
      end
      private_class_method :record_active_request

      def monotonic_seconds
        Process.clock_gettime(Process::CLOCK_MONOTONIC)
      end
      private_class_method :monotonic_seconds

      def integer_or_nil(value)
        return nil if value.nil? || value.to_s.empty?

        Integer(value)
      rescue ArgumentError
        nil
      end
      private_class_method :integer_or_nil

      def empty_to_nil(value)
        # rubocop:disable Rails/Blank
        value.nil? || value.empty? ? nil : value
        # rubocop:enable Rails/Blank
      end
      private_class_method :empty_to_nil

      def compact_attributes(attributes)
        attributes.compact
      end
      private_class_method :compact_attributes

      def write_trace_state(enabled:, available:, tracer:, meter:, provider:)
        TRACE_MUTEX.synchronize do
          TRACE_STATE.enabled = enabled
          TRACE_STATE.available = available
          TRACE_STATE.tracer = tracer
          TRACE_STATE.meter = meter
          TRACE_STATE.provider = provider
          TRACE_STATE.request_counter = nil unless meter
          TRACE_STATE.request_duration = nil unless meter
          TRACE_STATE.active_requests = nil unless meter
          TRACE_STATE.metric_instruments = {} unless meter
        end
      end
      private_class_method :write_trace_state

      def native_request_attributes(event)
        protocol_name, protocol_version = protocol_attributes('SERVER_PROTOCOL' => event[:protocol])
        compact_attributes(
          'http.request.method' => empty_to_nil(event[:method].to_s),
          'url.path' => request_path(event[:target].to_s),
          'http.response.status_code' => integer_or_nil(event[:status]),
          'server.address' => empty_to_nil(event[:host].to_s),
          'network.protocol.name' => protocol_name,
          'network.protocol.version' => protocol_version,
          'vajra.request.outcome' => event[:outcome],
          'vajra.failure.kind' => empty_to_nil(event[:failure_kind].to_s),
          'vajra.response.sent' => event[:response_sent],
          'vajra.connection.outcome' => empty_to_nil(event[:connection_outcome].to_s),
          'vajra.worker.index' => integer_or_nil(event[:worker_index]),
          'vajra.worker.pid' => integer_or_nil(event[:worker_pid])
        )
      end
      private_class_method :native_request_attributes

      def native_traceparent(event)
        trace_id = event[:trace_id].to_s
        span_id = event[:span_id].to_s
        trace_id.empty? || span_id.empty? ? nil : "00-#{trace_id}-#{span_id}-01"
      end
      private_class_method :native_traceparent

      def native_trace_env(event)
        { 'HTTP_TRACEPARENT' => native_traceparent(event) }
      end
      private_class_method :native_trace_env

      def native_parent_context(event)
        traceparent = native_traceparent(event)
        return nil unless traceparent

        extract_context('HTTP_TRACEPARENT' => traceparent)
      end
      private_class_method :native_parent_context

      def native_request_span_name(event)
        method = event[:method].to_s
        method.empty? ? 'HTTP request' : method
      end
      private_class_method :native_request_span_name

      def native_success_event?(event)
        status = integer_or_nil(event[:status])
        event[:response_sent] && status && status < 500 && event[:failure_kind].to_s.empty?
      end
      private_class_method :native_success_event?

      def request_path(target)
        path = target.split('?', 2).first
        empty_to_nil(path)
      end
      private_class_method :request_path

      def nanoseconds_to_seconds(value)
        nanoseconds = integer_or_nil(value)
        return 0.0 unless nanoseconds

        nanoseconds / 1_000_000_000.0
      end
      private_class_method :nanoseconds_to_seconds

      def lifecycle_attributes(event)
        compact_attributes(
          'vajra.worker.index' => event[:worker_index],
          'vajra.worker.pid' => event[:pid],
          'vajra.worker.lifecycle_state' => event[:lifecycle_state],
          'vajra.worker.health_state' => event[:health_state],
          'vajra.worker.recovery_state' => event[:recovery_state],
          'vajra.worker.exit_classification' => event[:exit_classification]
        )
      end
      private_class_method :lifecycle_attributes

      def emit_missing_dependency_warning
        TRACE_MUTEX.synchronize do
          return if TRACE_STATE.warning_emitted

          TRACE_STATE.warning_emitted = true
          warn('[Vajra][error] OpenTelemetry tracing requested, but required gems are not installed; tracing disabled')
        end
      end
      private_class_method :emit_missing_dependency_warning

      def update_native_status(config)
        __native_set_tracing_status__(
          config.fetch(:enabled),
          config.fetch(:available),
          config.fetch(:endpoint),
          config.fetch(:service_name),
          config.fetch(:active_context_required, false),
          config.fetch(:sample_ratio, 1.0)
        )
      end
      private_class_method :update_native_status

      def install_lifecycle_callback(callback)
        __native_set_lifecycle_callback__(callback)
      end
      private_class_method :install_lifecycle_callback

      def install_request_observability_callback(callback)
        return unless respond_to?(:__native_set_request_observability_callback__)

        __native_set_request_observability_callback__(callback)
        callback ? start_request_observability_drain_thread : stop_request_observability_drain_thread
      end
      private_class_method :install_request_observability_callback

      def start_request_observability_drain_thread
        return unless respond_to?(:__native_drain_request_observability_events__)

        stop_request_observability_drain_thread
        # rubocop:disable ThreadSafety/NewThread
        thread = Thread.new { request_observability_drain_loop }
        # rubocop:enable ThreadSafety/NewThread
        TRACE_MUTEX.synchronize do
          TRACE_STATE.request_observability_stop = false
          TRACE_STATE.request_observability_thread = thread
        end
      end
      private_class_method :start_request_observability_drain_thread

      def stop_request_observability_drain_thread
        thread = TRACE_MUTEX.synchronize do
          TRACE_STATE.request_observability_stop = true
          TRACE_STATE.request_observability_thread
        end
        if thread
          thread.wakeup if thread.alive?
          thread.join unless thread == Thread.current
        end
        drain_request_observability_batch until drain_request_observability_batch.zero?
        TRACE_MUTEX.synchronize do
          TRACE_STATE.request_observability_thread = nil
          TRACE_STATE.request_observability_stop = false
        end
      end
      private_class_method :stop_request_observability_drain_thread

      def request_observability_stop?
        TRACE_MUTEX.synchronize { TRACE_STATE.request_observability_stop }
      end
      private_class_method :request_observability_stop?

      def request_observability_drain_loop
        thread = Thread.current
        thread.abort_on_exception = false
        thread.priority = -5
        loop do
          break if request_observability_stop?

          drained = drain_request_observability_batch
          sleep(drained.zero? ? REQUEST_OBSERVABILITY_POLL_SECONDS : REQUEST_OBSERVABILITY_YIELD_SECONDS)
        end
        drain_request_observability_batch until drain_request_observability_batch.zero?
      end
      private_class_method :request_observability_drain_loop

      def drain_request_observability_batch
        events = drain_native_request_observability_events
        events.each { |event| emit_native_request_span(event) }
        events.size
      rescue StandardError
        0
      end
      private_class_method :drain_request_observability_batch

      def drain_native_request_observability_events
        __native_drain_request_observability_events__(REQUEST_OBSERVABILITY_BATCH_SIZE)
      end
      private_class_method :drain_native_request_observability_events
    end
    # rubocop:enable Metrics/ModuleLength
  end
end
