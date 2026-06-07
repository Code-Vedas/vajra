# frozen_string_literal: true

# Copyright Codevedas Inc. 2025-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

require 'spec_helper'

RSpec.describe Vajra::Internal::Tracing do
  let(:tracer) { Object.new }

  def stub_open_telemetry_classes(provider:, exporter: nil, processor: nil, batch_processor: true) # rubocop:disable Metrics/AbcSize
    open_telemetry_module = Module.new
    open_telemetry_module.singleton_class.attr_accessor :tracer_provider
    open_telemetry_module.tracer_provider = provider
    sdk_module = Module.new
    trace_module = Module.new
    export_module = Module.new
    exporter_module = Module.new
    otlp_module = Module.new

    tracer_provider_class = Class.new do
      define_singleton_method(:new) { provider }
    end
    simple_span_processor_class = Class.new do
      define_singleton_method(:new) { |arg| processor || arg }
    end
    batch_span_processor_class = Class.new do
      define_singleton_method(:new) { |arg| processor || arg }
    end
    otlp_exporter_class = Class.new do
      define_singleton_method(:new) { |endpoint:| exporter || endpoint }
    end

    export_module.const_set(:SimpleSpanProcessor, simple_span_processor_class)
    export_module.const_set(:BatchSpanProcessor, batch_span_processor_class) if batch_processor
    trace_module.const_set(:TracerProvider, tracer_provider_class)
    trace_module.const_set(:Export, export_module)
    sdk_module.const_set(:Trace, trace_module)
    otlp_module.const_set(:Exporter, otlp_exporter_class)
    exporter_module.const_set(:OTLP, otlp_module)
    open_telemetry_module.const_set(:SDK, sdk_module)
    open_telemetry_module.const_set(:Exporter, exporter_module)

    stub_const('OpenTelemetry', open_telemetry_module)
  end

  def native_metric_instrument
    Class.new do
      attr_reader :values

      def initialize
        @values = []
      end

      def add(value, attributes:)
        @values << [value, attributes]
      end

      def record(value, attributes:)
        @values << [value, attributes]
      end
    end.new
  end

  def native_error_span
    Class.new do
      attr_reader :attributes
      attr_accessor :status

      def initialize
        @attributes = {}
      end

      def set_attribute(name, value)
        @attributes[name] = value
      end
    end.new
  end

  def stub_error_status
    trace_module = Module.new
    status_class = Class.new do
      define_singleton_method(:error) { |message| [:error, message] }
    end
    trace_module.const_set(:Status, status_class)
    stub_const('OpenTelemetry::Trace', trace_module)
  end

  before do
    described_class::TRACE_MUTEX.synchronize do
      described_class::TRACE_STATE.enabled = false
      described_class::TRACE_STATE.available = false
      described_class::TRACE_STATE.tracer = nil
      described_class::TRACE_STATE.meter = nil
      described_class::TRACE_STATE.provider = nil
      described_class::TRACE_STATE.warning_emitted = false
      described_class::TRACE_STATE.request_counter = nil
      described_class::TRACE_STATE.request_duration = nil
      described_class::TRACE_STATE.active_requests = nil
      described_class::TRACE_STATE.metric_instruments = {}
    end
    allow(described_class).to receive(:require).and_call_original
    allow(Kernel).to receive(:require).and_call_original
    allow(described_class).to receive(:__native_set_tracing_status__)
    allow(described_class).to receive(:__native_set_lifecycle_callback__)
    allow(described_class).to receive(:__native_set_request_observability_callback__)
    allow(described_class).to receive(:warn)
  end

  after do
    described_class.send(:write_trace_state, enabled: false, available: false, tracer: nil, meter: nil, provider: nil)
    described_class::TRACE_MUTEX.synchronize do
      described_class::TRACE_STATE.warning_emitted = false
    end
    ENV.delete('VAJRA_TRACE_ENABLED')
    ENV.delete('VAJRA_TRACE_ENDPOINT')
    ENV.delete('VAJRA_TRACE_SERVICE_NAME')
    ENV.delete('VAJRA_TRACE_OTEL_OWNER')
    ENV.delete('OTEL_SERVICE_NAME')
    ENV.delete('OTEL_EXPORTER_OTLP_ENDPOINT')
    ENV.delete('OTEL_TRACES_EXPORTER')
    ENV.delete('OTEL_METRICS_EXPORTER')
    ENV.delete('OTEL_PROPAGATORS')
    ENV.delete('OTEL_RESOURCE_ATTRIBUTES')
    ENV.delete('OTEL_TRACES_SAMPLER')
    ENV.delete('OTEL_TRACES_SAMPLER_ARG')
  end

  it 'disables tracing cleanly when trace_enabled is false' do
    expect(
      described_class.install_from_start_options!(
        trace_enabled: false,
        trace_endpoint: '/ignored',
        trace_service_name: 'vajra-test'
      )
    ).to be(false)

    expect(described_class).to have_received(:__native_set_tracing_status__)
      .with(false, false, '/ignored', 'vajra-test')
    expect(described_class).to have_received(:__native_set_lifecycle_callback__).with(nil)
    expect(described_class).to have_received(:__native_set_request_observability_callback__).with(nil)
  end

  it 'warns and stays boot-safe when OpenTelemetry gems are unavailable' do
    allow(Kernel).to receive(:require).with('opentelemetry/sdk').and_raise(LoadError)

    expect(
      described_class.install_from_start_options!(
        trace_enabled: true,
        trace_endpoint: 'http://127.0.0.1:4318/v1/traces',
        trace_service_name: 'vajra-test'
      )
    ).to be(false)

    expect(described_class).to have_received(:__native_set_tracing_status__)
      .with(true, false, 'http://127.0.0.1:4318/v1/traces', 'vajra-test')
    expect(described_class).to have_received(:warn).with(include('OpenTelemetry tracing requested'))
  end

  it 'installs lifecycle tracing when a tracer is available' do
    allow(described_class).to receive(:build_telemetry).and_return(provider: nil, tracer: tracer, meter: nil)

    expect(
      described_class.install_from_start_options!(
        trace_enabled: true,
        trace_endpoint: 'http://127.0.0.1:4318/v1/traces',
        trace_service_name: 'vajra-test'
      )
    ).to be(true)

    expect(described_class).to have_received(:__native_set_tracing_status__)
      .with(true, true, 'http://127.0.0.1:4318/v1/traces', 'vajra-test')
    expect(described_class).to have_received(:__native_set_lifecycle_callback__).with(described_class.method(:emit_lifecycle_span))
    expect(described_class).to have_received(:__native_set_request_observability_callback__)
      .with(described_class.method(:emit_native_request_span))
  end

  it 'yields directly when request tracing is unavailable' do
    seen = false

    described_class.with_request_span('REQUEST_METHOD' => 'GET', 'PATH_INFO' => '/plain') do
      seen = true
      :ok
    end

    expect(seen).to be(true)
  end

  it 'wraps request execution in a tracer span when tracing is available' do
    described_class.send(:write_trace_state, enabled: true, available: true, tracer: tracer, meter: nil, provider: nil)
    allow(tracer).to receive(:in_span).and_yield(nil)

    described_class.with_request_span(
      'REQUEST_METHOD' => 'POST',
      'PATH_INFO' => '/widgets',
      'rack.url_scheme' => 'http',
      'SERVER_NAME' => 'example.test',
      'SERVER_PORT' => '3000',
      'SERVER_PROTOCOL' => 'HTTP/1.1'
    ) { :ok }

    expect(tracer).to have_received(:in_span).with(
      'POST',
      attributes: {
        'http.request.method' => 'POST',
        'url.path' => '/widgets',
        'url.scheme' => 'http',
        'server.address' => 'example.test',
        'server.port' => 3000,
        'network.protocol.name' => 'http',
        'network.protocol.version' => '1.1'
      }
    )
  end

  it 'passes extracted parent context and status-tagged metrics through request spans' do
    propagation = Class.new do
      def extract(_carrier)
        :parent_context
      end
    end.new
    stub_const('OpenTelemetry', Module.new)
    OpenTelemetry.singleton_class.define_method(:propagation) { propagation }
    request_counter = Class.new do
      attr_reader :entries

      def initialize
        @entries = []
      end

      def add(value, attributes:)
        @entries << [value, attributes]
      end
    end.new
    request_duration = Class.new do
      attr_reader :entries

      def initialize
        @entries = []
      end

      def record(value, attributes:)
        @entries << [value, attributes]
      end
    end.new
    described_class.send(:write_trace_state, enabled: true, available: true, tracer:, meter: Object.new, provider: nil)
    described_class::TRACE_MUTEX.synchronize do
      described_class::TRACE_STATE.request_counter = request_counter
      described_class::TRACE_STATE.request_duration = request_duration
    end
    allow(tracer).to receive(:in_span).and_yield(Object.new)

    response = described_class.with_request_span(
      'REQUEST_METHOD' => 'GET',
      'SERVER_PROTOCOL' => 'HTTP/1.1',
      'HTTP_TRACEPARENT' => 'trace'
    ) { [204, {}, []] }

    expect(response).to eq([204, {}, []])
    expect(tracer).to have_received(:in_span).with(
      'GET',
      attributes: hash_including('http.request.method' => 'GET'),
      with_parent: :parent_context
    )
    expect(request_counter.entries.last.last).to include('http.response.status_code' => 204)
    expect(request_duration.entries.last.last).to include('http.response.status_code' => 204)
  end

  it 'attaches active span ids to Rack response headers for native log correlation' do
    context = Class.new do
      def hex_trace_id
        'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'
      end

      def hex_span_id
        'bbbbbbbbbbbbbbbb'
      end
    end.new
    span = Class.new do
      define_method(:initialize) { |context| @context = context }
      define_method(:context) { @context }
    end.new(context)
    described_class.send(:write_trace_state, enabled: true, available: true, tracer:, meter: nil, provider: nil)
    allow(tracer).to receive(:in_span).and_yield(span)

    response = described_class.with_request_span('REQUEST_METHOD' => 'GET') { [200, {}, ['OK']] }

    expect(response[1]).to include(
      described_class::INTERNAL_TRACE_ID_HEADER => 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',
      described_class::INTERNAL_SPAN_ID_HEADER => 'bbbbbbbbbbbbbbbb'
    )
  end

  it 'attaches active span ids to array response headers' do
    context = Class.new do
      def trace_id
        'cccccccccccccccccccccccccccccccc'
      end

      def span_id
        'dddddddddddddddd'
      end
    end.new
    span = Class.new do
      define_method(:initialize) { |context| @context = context }
      define_method(:span_context) { @context }
    end.new(context)
    described_class.send(:write_trace_state, enabled: true, available: true, tracer:, meter: nil, provider: nil)
    allow(tracer).to receive(:in_span).and_yield(span)

    response = described_class.with_request_span('REQUEST_METHOD' => 'GET') { [200, [], ['OK']] }

    expect(response[1]).to include(
      [described_class::INTERNAL_TRACE_ID_HEADER, 'cccccccccccccccccccccccccccccccc'],
      [described_class::INTERNAL_SPAN_ID_HEADER, 'dddddddddddddddd']
    )
  end

  it 'leaves response headers untouched when span ids are unavailable' do
    described_class.send(:write_trace_state, enabled: true, available: true, tracer:, meter: nil, provider: nil)
    allow(tracer).to receive(:in_span).and_yield(Object.new)

    response = described_class.with_request_span('REQUEST_METHOD' => 'GET') { [200, Object.new, ['OK']] }

    expect(response[1]).not_to respond_to(:[])
  end

  it 'handles partial and unusable trace context during response correlation' do
    trace_only_context = Class.new do
      def hex_trace_id
        'eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee'
      end
    end.new
    span_only_context = Class.new do
      def hex_span_id
        'ffffffffffffffff'
      end
    end.new
    nil_context = Class.new do
      def hex_trace_id
        nil
      end
    end.new

    trace_only_span = Class.new do
      define_method(:initialize) { |context| @context = context }
      define_method(:context) { @context }
    end.new(trace_only_context)
    span_only_span = Class.new do
      define_method(:initialize) { |context| @context = context }
      define_method(:context) { @context }
    end.new(span_only_context)
    nil_span = Class.new do
      define_method(:initialize) { |context| @context = context }
      define_method(:context) { @context }
    end.new(nil_context)

    described_class.send(:write_trace_state, enabled: true, available: true, tracer:, meter: nil, provider: nil)
    spans = [trace_only_span, span_only_span, trace_only_span, span_only_span, nil_span, trace_only_span]
    allow(tracer).to receive(:in_span) do |_name, **_options, &block|
      block.call(spans.shift)
    end

    hash_response = described_class.with_request_span('REQUEST_METHOD' => 'GET') { [200, {}, ['OK']] }
    array_response = described_class.with_request_span('REQUEST_METHOD' => 'GET') { [200, [], ['OK']] }
    array_trace_only_response = described_class.with_request_span('REQUEST_METHOD' => 'GET') { [200, [], ['OK']] }
    hash_span_only_response = described_class.with_request_span('REQUEST_METHOD' => 'GET') { [200, {}, ['OK']] }
    nil_context_response = described_class.with_request_span('REQUEST_METHOD' => 'GET') { [200, {}, ['OK']] }
    object_header_response = described_class.with_request_span('REQUEST_METHOD' => 'GET') { [200, Object.new, ['OK']] }

    expect(hash_response[1]).to include(described_class::INTERNAL_TRACE_ID_HEADER => 'eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee')
    expect(hash_response[1]).not_to include(described_class::INTERNAL_SPAN_ID_HEADER)
    expect(array_response[1]).to include([described_class::INTERNAL_SPAN_ID_HEADER, 'ffffffffffffffff'])
    expect(array_response[1]).not_to include([described_class::INTERNAL_TRACE_ID_HEADER, nil])
    expect(array_trace_only_response[1]).to include([described_class::INTERNAL_TRACE_ID_HEADER, 'eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee'])
    expect(array_trace_only_response[1]).not_to include([described_class::INTERNAL_SPAN_ID_HEADER, nil])
    expect(hash_span_only_response[1]).to include(described_class::INTERNAL_SPAN_ID_HEADER => 'ffffffffffffffff')
    expect(hash_span_only_response[1]).not_to include(described_class::INTERNAL_TRACE_ID_HEADER)
    expect(nil_context_response[1]).to be_empty
    expect(object_header_response[1]).not_to respond_to(:[])
  end

  it 'emits lifecycle spans when tracing is available' do
    described_class.send(:write_trace_state, enabled: true, available: true, tracer: tracer, meter: nil, provider: nil)
    allow(tracer).to receive(:in_span).and_yield(nil)

    described_class.emit_lifecycle_span(
      event: 'worker_replacement_ready',
      worker_index: 1,
      pid: 123,
      lifecycle_state: 'ready',
      health_state: 'healthy',
      recovery_state: 'none',
      available: true,
      exit_classification: 'none',
      replacement_needed: false,
      terminal_replacement_failure: false,
      exit_detail: 0
    )

    expect(tracer).to have_received(:in_span).with(
      'vajra.worker_replacement_ready',
      attributes: hash_including(
        'vajra.worker.index' => 1,
        'vajra.worker.pid' => 123,
        'vajra.worker.health_state' => 'healthy'
      )
    )
  end

  it 'skips lifecycle spans when tracing is unavailable' do
    expect do
      described_class.emit_lifecycle_span(event: 'worker_ready')
    end.not_to raise_error
  end

  it 'uses an app-owned tracer provider unless Vajra is configured to own OTel setup' do
    provider_class = Class.new do
      attr_reader :processors

      def initialize(tracer)
        @tracer = tracer
        @processors = []
      end

      def tracer(_name)
        @tracer
      end

      def add_span_processor(processor)
        @processors << processor
      end
    end
    provider = provider_class.new(tracer)
    stub_open_telemetry_classes(provider:)
    allow(described_class).to receive(:require).with('opentelemetry/sdk').and_return(true)
    allow(Kernel).to receive(:require).with('opentelemetry/sdk').and_return(true)

    config = described_class.send(
      :resolve_config,
      trace_enabled: true,
      trace_service_name: 'vajra-test'
    )
    telemetry = described_class.send(:build_telemetry, config)

    expect(telemetry.fetch(:tracer)).to eq(tracer)
    expect(provider.processors).to be_empty
  end

  it 'returns nil tracer when an app-owned global provider is unset' do
    stub_open_telemetry_classes(provider: nil)
    allow(described_class).to receive(:require).with('opentelemetry/sdk').and_return(true)

    config = described_class.send(:resolve_config, trace_enabled: true)
    telemetry = described_class.send(:build_telemetry, config)

    expect(telemetry).to include(provider: nil, tracer: nil, meter: nil)
  end

  it 'handles OpenTelemetry modules without a global app provider' do
    open_telemetry_module = Module.new
    sdk_module = Module.new
    trace_module = Module.new
    provider = Class.new do
      def tracer(_name)
        Object.new
      end
    end.new
    trace_module.const_set(:TracerProvider, Class.new do
      define_singleton_method(:new) { provider }
    end)
    sdk_module.const_set(:Trace, trace_module)
    open_telemetry_module.const_set(:SDK, sdk_module)
    stub_const('OpenTelemetry', open_telemetry_module)
    allow(described_class).to receive(:require).with('opentelemetry/sdk').and_return(true)

    config = described_class.send(
      :resolve_config,
      trace_enabled: true,
      trace_endpoint: '',
      trace_otel_owner: true
    )
    telemetry = described_class.send(:build_telemetry, config)

    expect(telemetry.fetch(:provider)).to eq(provider)
    expect(telemetry.fetch(:tracer)).not_to be_nil
  end

  it 'builds an owned tracer with a BatchSpanProcessor when configured to own OTel setup' do
    provider_class = Class.new do
      attr_reader :processors

      def initialize(tracer)
        @tracer = tracer
        @processors = []
      end

      def tracer(_name)
        @tracer
      end

      def add_span_processor(processor)
        @processors << processor
      end
    end
    provider = provider_class.new(tracer)
    exporter = Object.new
    processor = Object.new
    stub_open_telemetry_classes(provider:, exporter:, processor:)
    allow(described_class).to receive(:require).with('opentelemetry/sdk').and_return(true)
    allow(described_class).to receive(:require).with('opentelemetry/exporter/otlp').and_return(true)
    allow(Kernel).to receive(:require).with('opentelemetry/sdk').and_return(true)
    allow(Kernel).to receive(:require).with('opentelemetry/exporter/otlp').and_return(true)

    config = described_class.send(
      :resolve_config,
      trace_enabled: true,
      trace_endpoint: 'http://127.0.0.1:4318/v1/traces',
      trace_service_name: 'vajra-test',
      trace_otel_owner: true
    )
    telemetry = described_class.send(:build_telemetry, config)

    expect(telemetry.fetch(:tracer)).to eq(tracer)
    expect(provider.processors).to eq([processor])
  end

  it 'resolves standard OTEL environment variables behind explicit Vajra options' do
    ENV['VAJRA_TRACE_ENABLED'] = 'true'
    ENV['OTEL_EXPORTER_OTLP_ENDPOINT'] = 'http://collector:4318'
    ENV['OTEL_SERVICE_NAME'] = 'otel-service'

    config = described_class.send(:resolve_config, trace_endpoint: 'http://explicit:4318')

    expect(config.enabled).to be(true)
    expect(config.endpoint).to eq('http://explicit:4318')
    expect(config.service_name).to eq('otel-service')
  end

  it 'treats blank environment values as unset and preserves OTEL metadata config' do
    ENV['VAJRA_TRACE_ENABLED'] = ' '
    ENV['OTEL_TRACES_EXPORTER'] = 'none'
    ENV['OTEL_EXPORTER_OTLP_ENDPOINT'] = ' '
    ENV['OTEL_PROPAGATORS'] = 'tracecontext'
    ENV['OTEL_RESOURCE_ATTRIBUTES'] = 'deployment.environment=test'
    ENV['OTEL_TRACES_SAMPLER_ARG'] = '0.25'

    config = described_class.send(:resolve_config, {})

    expect(config.enabled).to be(false)
    expect(config.endpoint).to eq('')
    expect(config.propagators).to eq('tracecontext')
    expect(config.resource_attributes).to eq('deployment.environment=test')
    expect(config.sampler).to eq('0.25')
  end

  it 'returns unavailable telemetry when the OTLP exporter dependency is unavailable' do
    provider_class = Class.new do
      attr_reader :processors

      def initialize
        @processors = []
      end

      def tracer(_name)
        Object.new
      end

      def add_span_processor(processor)
        @processors << processor
      end
    end
    provider = provider_class.new
    stub_open_telemetry_classes(provider:)
    allow(described_class).to receive(:require).with('opentelemetry/sdk').and_return(true)
    allow(described_class).to receive(:require).with('opentelemetry/exporter/otlp').and_raise(LoadError)
    allow(Kernel).to receive(:require).with('opentelemetry/sdk').and_return(true)
    allow(Kernel).to receive(:require).with('opentelemetry/exporter/otlp').and_raise(LoadError)

    config = described_class.send(
      :resolve_config,
      trace_enabled: true,
      trace_endpoint: 'http://127.0.0.1:4318/v1/traces',
      trace_service_name: 'vajra-test',
      trace_otel_owner: true
    )
    telemetry = described_class.send(:build_telemetry, config)

    expect(telemetry.fetch(:tracer)).not_to be_nil
    expect(provider.processors).to be_empty
    expect(described_class).to have_received(:warn).with(include('OpenTelemetry tracing requested'))
  end

  it 'enables tracing from OTEL_TRACES_EXPORTER and rejects invalid booleans' do
    ENV['OTEL_TRACES_EXPORTER'] = 'otlp'

    config = described_class.send(:resolve_config, {})

    expect(config.enabled).to be(true)
    expect do
      described_class.send(:boolean_value, 'maybe', 'VAJRA_TRACE_ENABLED')
    end.to raise_error(ArgumentError, /invalid VAJRA_TRACE_ENABLED/)
  end

  it 'falls back to SimpleSpanProcessor when BatchSpanProcessor is unavailable' do
    provider_class = Class.new do
      attr_reader :processors

      def initialize(tracer)
        @tracer = tracer
        @processors = []
      end

      def tracer(_name)
        @tracer
      end

      def add_span_processor(processor)
        @processors << processor
      end
    end
    provider = provider_class.new(tracer)
    processor = Object.new
    stub_open_telemetry_classes(provider:, processor:, batch_processor: false)
    allow(described_class).to receive(:require).with('opentelemetry/sdk').and_return(true)
    allow(described_class).to receive(:require).with('opentelemetry/exporter/otlp').and_return(true)

    config = described_class.send(
      :resolve_config,
      trace_enabled: true,
      trace_endpoint: 'http://127.0.0.1:4318/v1/traces',
      trace_otel_owner: true
    )
    telemetry = described_class.send(:build_telemetry, config)

    expect(telemetry.fetch(:tracer)).to eq(tracer)
    expect(provider.processors).to eq([processor])
  end

  it 'skips exporter setup when tracing export is disabled for an owned provider' do
    provider_class = Class.new do
      attr_reader :processors

      def initialize(tracer)
        @tracer = tracer
        @processors = []
      end

      def tracer(_name)
        @tracer
      end

      def add_span_processor(processor)
        @processors << processor
      end
    end
    provider = provider_class.new(tracer)
    stub_open_telemetry_classes(provider:)
    allow(described_class).to receive(:require).with('opentelemetry/sdk').and_return(true)
    ENV['OTEL_TRACES_EXPORTER'] = 'none'

    config = described_class.send(
      :resolve_config,
      trace_enabled: true,
      trace_endpoint: 'http://127.0.0.1:4318/v1/traces',
      trace_otel_owner: true
    )
    telemetry = described_class.send(:build_telemetry, config)

    expect(telemetry.fetch(:tracer)).to eq(tracer)
    expect(provider.processors).to be_empty
  end

  it 'installs request metric instruments when a meter provider is configured' do
    ENV['OTEL_METRICS_EXPORTER'] = 'otlp'
    instruments = []
    meter = Class.new do
      define_method(:initialize) { |store| @store = store }
      define_method(:create_counter) { |name, unit:| @store << [:counter, name, unit] }
      define_method(:create_histogram) { |name, unit:| @store << [:histogram, name, unit] }
      define_method(:create_up_down_counter) { |name, unit:| @store << [:up_down_counter, name, unit] }
    end.new(instruments)
    meter_provider = Class.new do
      define_method(:initialize) { |meter| @meter = meter }
      define_method(:meter) { |_service_name| @meter }
    end.new(meter)
    provider = Class.new do
      define_method(:tracer) { |_name| Object.new }
    end.new
    stub_open_telemetry_classes(provider:)
    OpenTelemetry.singleton_class.attr_accessor :meter_provider
    OpenTelemetry.meter_provider = meter_provider
    allow(described_class).to receive(:require).with('opentelemetry/sdk').and_return(true)

    config = described_class.send(
      :resolve_config,
      trace_enabled: true
    )
    telemetry = described_class.send(:build_telemetry, config)
    described_class.send(:write_trace_state, enabled: true, available: true, tracer:, meter: telemetry.fetch(:meter), provider:)
    described_class.send(:install_metric_instruments)

    expect(instruments).to include(
      [:counter, 'vajra.http.server.requests', '1'],
      [:histogram, 'vajra.http.server.duration', 's'],
      [:up_down_counter, 'vajra.http.server.active_requests', '1'],
      [:counter, 'vajra.native.request.outcomes', '1'],
      [:counter, 'vajra.request.admission.outcomes', '1'],
      [:counter, 'vajra.worker.lifecycle.events', '1'],
      [:up_down_counter, 'vajra.worker.active_executions', '1']
    )
  end

  it 'returns no meter when metrics are enabled without a meter provider' do
    ENV['OTEL_METRICS_EXPORTER'] = 'otlp'
    stub_open_telemetry_classes(provider: Object.new)

    config = described_class.send(:resolve_config, trace_enabled: true)

    expect(described_class.send(:configured_meter, config)).to be_nil
  end

  it 'handles propagation extraction and fallback request span names' do
    propagation = Class.new do
      attr_reader :carrier

      def extract(carrier)
        @carrier = carrier
        :context
      end
    end.new
    stub_const('OpenTelemetry', Module.new)
    OpenTelemetry.singleton_class.define_method(:propagation) { propagation }

    expect(described_class.send(:request_span_name, 'REQUEST_METHOD' => '')).to eq('HTTP request')
    expect(described_class.send(:protocol_attributes, 'SERVER_PROTOCOL' => 'WEBSOCKET')).to eq(['http', nil])
    expect(
      described_class.send(:extract_context, 'HTTP_TRACEPARENT' => 'trace', 'HTTP_TRACESTATE' => 'state')
    ).to eq(:context)
    expect(propagation.carrier).to eq('traceparent' => 'trace', 'tracestate' => 'state')
  end

  it 'records span response status and exceptions when span APIs are available' do
    span = Class.new do
      attr_reader :attributes, :exception
      attr_accessor :status

      def initialize
        @attributes = {}
      end

      def set_attribute(name, value)
        @attributes[name] = value
      end

      def record_exception(error)
        @exception = error
      end
    end.new
    trace_module = Module.new
    status_class = Class.new do
      define_singleton_method(:ok) { :ok }
      define_singleton_method(:error) { |message| [:error, message] }
    end
    trace_module.const_set(:Status, status_class)
    trace_module.singleton_class.define_method(:current_span) { span }
    stub_const('OpenTelemetry::Trace', trace_module)

    described_class.send(:record_span_response_status, span, 201)
    described_class.send(:mark_span_success, span)
    error = RuntimeError.new('boom')
    described_class.send(:record_span_exception, error)

    expect(span.attributes).to include('http.response.status_code' => 201)
    expect(span.status).to eq([:error, 'boom'])
    expect(span.exception).to eq(error)
  end

  it 'emits native request spans and metrics for native-only request failures' do
    instrument = native_metric_instrument
    span = native_error_span
    stub_error_status
    described_class.send(:write_trace_state, enabled: true, available: true, tracer:, meter: Object.new, provider: nil)
    described_class::TRACE_MUTEX.synchronize do
      described_class::TRACE_STATE.request_counter = instrument
      described_class::TRACE_STATE.request_duration = instrument
      described_class::TRACE_STATE.metric_instruments = {
        native_request_counter: instrument,
        native_error_counter: instrument,
        admission_counter: instrument
      }
    end
    allow(tracer).to receive(:in_span).and_yield(span)

    described_class.emit_native_request_span(
      method: 'GET',
      target: '/queue',
      protocol: 'HTTP/1.1',
      host: 'example.test',
      status: 503,
      duration_nanoseconds: 250_000_000,
      outcome: 'queue_capacity',
      failure_kind: 'queue_capacity',
      response_sent: true,
      connection_outcome: 'close',
      worker_index: 0,
      worker_pid: 123,
      trace_id: '11111111111111111111111111111111',
      span_id: '2222222222222222',
      error_message: 'full'
    )

    expect(tracer).to have_received(:in_span).with(
      'GET',
      attributes: hash_including(
        'http.response.status_code' => 503,
        'vajra.request.outcome' => 'queue_capacity',
        'vajra.failure.kind' => 'queue_capacity',
        'vajra.response.sent' => true
      )
    ).at_least(:once)
    expect(span.status).to eq([:error, 'full'])
    expect(instrument.values).to include([1, hash_including('vajra.request.outcome' => 'queue_capacity')])
  end

  it 'emits native request success spans with parent context' do
    propagation = Class.new do
      def extract(_carrier)
        :native_parent
      end
    end.new
    span = Class.new do
      attr_accessor :status

      def set_attribute(_name, _value); end
    end.new
    trace_module = Module.new
    status_class = Class.new do
      define_singleton_method(:ok) { :ok }
    end
    trace_module.const_set(:Status, status_class)
    stub_const('OpenTelemetry::Trace', trace_module)
    OpenTelemetry.singleton_class.define_method(:propagation) { propagation }
    described_class.send(:write_trace_state, enabled: true, available: true, tracer:, meter: nil, provider: nil)
    allow(tracer).to receive(:in_span).and_yield(span)

    described_class.emit_native_request_span(
      method: '',
      target: '',
      protocol: '',
      status: 204,
      duration_nanoseconds: nil,
      outcome: 'completed',
      failure_kind: '',
      response_sent: true,
      trace_id: '11111111111111111111111111111111',
      span_id: '2222222222222222'
    )

    expect(tracer).to have_received(:in_span).with(
      'HTTP request',
      attributes: hash_including('http.response.status_code' => 204, 'vajra.request.outcome' => 'completed'),
      with_parent: :native_parent
    )
    expect(span.status).to eq(:ok)
  end

  it 'skips native request spans when tracing is unavailable' do
    described_class.send(:write_trace_state, enabled: false, available: false, tracer: nil, meter: nil, provider: nil)

    expect do
      described_class.emit_native_request_span(method: 'GET')
    end.not_to raise_error
  end

  it 'skips native request callback installation when the extension does not expose it' do
    allow(described_class).to receive(:respond_to?)
      .with(:__native_set_request_observability_callback__)
      .and_return(false)

    expect do
      described_class.send(:install_request_observability_callback, proc {})
    end.not_to raise_error
  end

  it 'falls back when named metric instruments do not accept attributes' do
    instrument = Class.new do
      attr_reader :values

      def initialize
        @values = []
      end

      def add(value, attributes: nil)
        raise ArgumentError if attributes

        @values << value
      end
    end.new
    described_class.send(:write_trace_state, enabled: true, available: true, tracer:, meter: Object.new, provider: nil)
    described_class::TRACE_MUTEX.synchronize do
      described_class::TRACE_STATE.metric_instruments = { native_request_counter: instrument }
    end

    described_class.send(:record_named_metric, :native_request_counter, 3, 'key' => 'value')
    described_class.send(:record_named_metric, :missing, 1, {})

    expect(instrument.values).to eq([3])
  end

  it 'ignores named metric fallback when lookup itself fails' do
    allow(described_class::TRACE_MUTEX).to receive(:synchronize).and_raise(ArgumentError)

    expect do
      described_class.send(:record_named_metric, :native_request_counter, 1, {})
    end.not_to raise_error
    allow(described_class::TRACE_MUTEX).to receive(:synchronize).and_call_original
  end

  it 'ignores spans and status APIs that are not available' do
    span = Class.new do
      attr_accessor :status
    end.new
    trace_module = Module.new
    stub_const('OpenTelemetry::Trace', trace_module)

    expect(described_class.send(:response_status_from, Object.new)).to be_nil
    expect(described_class.send(:current_span)).to be_nil
    described_class.send(:mark_span_success, span)
    described_class.send(:record_span_exception, RuntimeError.new('boom'))

    expect(span.status).to be_nil
  end

  it 'sets error status without recording an exception when only status API exists' do
    span = Class.new do
      attr_accessor :status
    end.new
    trace_module = Module.new
    status_class = Class.new do
      define_singleton_method(:error) { |message| [:error, message] }
    end
    trace_module.const_set(:Status, status_class)
    trace_module.singleton_class.define_method(:current_span) { span }
    stub_const('OpenTelemetry::Trace', trace_module)

    described_class.send(:record_span_exception, RuntimeError.new('status only'))

    expect(span.status).to eq([:error, 'status only'])
  end

  it 'falls back when metric instruments do not accept attributes' do
    instrument = Class.new do
      attr_reader :values

      def initialize
        @values = []
      end

      def add(value, attributes: nil)
        raise ArgumentError if attributes

        @values << [:add, value]
      end

      def record(value, attributes: nil)
        raise ArgumentError if attributes

        @values << [:record, value]
      end
    end.new

    described_class.send(
      :write_trace_state,
      enabled: true,
      available: true,
      tracer:,
      meter: nil,
      provider: nil
    )
    described_class::TRACE_MUTEX.synchronize do
      described_class::TRACE_STATE.request_counter = instrument
      described_class::TRACE_STATE.request_duration = instrument
      described_class::TRACE_STATE.active_requests = instrument
    end

    described_class.send(:record_request_metrics, 0.25, 'http.request.method' => 'GET')
    described_class.send(:record_active_request, 1, 'http.request.method' => 'GET')

    expect(instrument.values).to include([:add, 1], [:record, 0.25])
  end

  it 'allows request metric instruments to be absent independently' do
    instrument = Class.new do
      attr_reader :values

      def initialize
        @values = []
      end

      def add(value, attributes:)
        @values << [value, attributes]
      end
    end.new
    described_class.send(:write_trace_state, enabled: true, available: true, tracer:, meter: Object.new, provider: nil)
    described_class::TRACE_MUTEX.synchronize do
      described_class::TRACE_STATE.request_counter = instrument
      described_class::TRACE_STATE.request_duration = nil
      described_class::TRACE_STATE.active_requests = nil
    end

    described_class.send(:record_request_metrics, 0.1, 'http.request.method' => 'GET')
    described_class.send(:record_active_request, 1, 'http.request.method' => 'GET')

    expect(instrument.values).to eq([[1, { 'http.request.method' => 'GET' }]])
  end

  it 'allows fallback request metric instruments to be absent independently' do
    instrument = Class.new do
      attr_reader :values

      def initialize
        @values = []
      end

      def record(value, attributes: nil)
        raise ArgumentError if attributes

        @values << value
      end
    end.new
    described_class.send(:write_trace_state, enabled: true, available: true, tracer:, meter: Object.new, provider: nil)
    described_class::TRACE_MUTEX.synchronize do
      described_class::TRACE_STATE.request_counter = nil
      described_class::TRACE_STATE.request_duration = instrument
      described_class::TRACE_STATE.active_requests = nil
    end

    described_class.send(:record_request_metrics, 0.1, 'http.request.method' => 'GET')

    expect(instrument.values).to eq([0.1])
  end

  it 'allows fallback request duration to be absent when counters reject attributes' do
    instrument = Class.new do
      attr_reader :values

      def initialize
        @values = []
      end

      def add(value, attributes: nil)
        raise ArgumentError if attributes

        @values << value
      end
    end.new
    described_class.send(:write_trace_state, enabled: true, available: true, tracer:, meter: Object.new, provider: nil)
    described_class::TRACE_MUTEX.synchronize do
      described_class::TRACE_STATE.request_counter = instrument
      described_class::TRACE_STATE.request_duration = nil
    end

    described_class.send(:record_request_metrics, 0.1, 'http.request.method' => 'GET')

    expect(instrument.values).to eq([1])
  end

  it 'allows active request fallback metrics to be absent after synchronization errors' do
    allow(described_class::TRACE_MUTEX).to receive(:synchronize).and_raise(ArgumentError)

    expect do
      described_class.send(:record_active_request, 1, 'http.request.method' => 'GET')
    end.not_to raise_error
    allow(described_class::TRACE_MUTEX).to receive(:synchronize).and_call_original
  end

  it 'records request span exceptions and re-raises them' do
    span = Class.new do
      attr_reader :exception

      def record_exception(error)
        @exception = error
      end
    end.new
    trace_module = Module.new
    trace_module.singleton_class.define_method(:current_span) { span }
    stub_const('OpenTelemetry::Trace', trace_module)
    described_class.send(:write_trace_state, enabled: true, available: true, tracer: tracer, meter: nil, provider: nil)
    allow(tracer).to receive(:in_span).and_yield(nil)

    expect do
      described_class.with_request_span('REQUEST_METHOD' => 'GET') { raise 'boom' }
    end.to raise_error(RuntimeError, 'boom')
    expect(span.exception.message).to eq('boom')
  end

  it 'falls back when instrument factories do not accept unit keywords' do
    meter = Class.new do
      def create_counter(name, unit: nil)
        raise ArgumentError if unit

        name
      end
    end.new

    expect(described_class.send(:build_instrument, meter, :create_counter, 'counter.name')).to eq('counter.name')
    expect(described_class.send(:build_instrument, Object.new, :create_counter, 'missing')).to be_nil
  end

  it 'ignores propagation extraction API failures' do
    propagation = Class.new do
      def extract(_carrier)
        raise NoMethodError
      end
    end.new
    stub_const('OpenTelemetry', Module.new)
    OpenTelemetry.singleton_class.define_method(:propagation) { propagation }

    expect(described_class.send(:extract_context, 'HTTP_TRACEPARENT' => 'trace')).to be_nil
  end

  it 'handles scalar parsing fallbacks' do
    expect(described_class.send(:integer_or_nil, 'invalid')).to be_nil
    expect(described_class.send(:empty_to_nil, '')).to be_nil
    expect(described_class.send(:empty_to_nil, 'value')).to eq('value')
    expect(described_class.send(:native_trace_env, trace_id: '', span_id: 'span')).to eq('HTTP_TRACEPARENT' => nil)
    expect(described_class.send(:response_status_from, ['not-a-status'])).to be_nil
  end

  it 'flushes and shuts down an owned provider on shutdown' do
    provider = Class.new do
      attr_reader :force_flushed, :shut_down

      def force_flush
        @force_flushed = true
      end

      def shutdown
        @shut_down = true
      end
    end.new
    described_class.send(:write_trace_state, enabled: true, available: true, tracer: tracer, meter: nil, provider:)

    described_class.shutdown!

    expect(provider.force_flushed).to be(true)
    expect(provider.shut_down).to be(true)
  end

  it 'shuts down cleanly when a provider has no lifecycle APIs' do
    provider = Object.new
    described_class.send(:write_trace_state, enabled: true, available: true, tracer: tracer, meter: nil, provider:)

    described_class.shutdown!

    expect(described_class::TRACE_MUTEX.synchronize { described_class::TRACE_STATE.provider }).to be_nil
  end

  it 'only warns once for missing tracing dependencies' do
    described_class.send(:emit_missing_dependency_warning)
    described_class.send(:emit_missing_dependency_warning)

    expect(described_class).to have_received(:warn).once
  end
end
