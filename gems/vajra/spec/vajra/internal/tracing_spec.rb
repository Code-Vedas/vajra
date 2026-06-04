# frozen_string_literal: true

# Copyright Codevedas Inc. 2025-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

require 'spec_helper'

RSpec.describe Vajra::Internal::Tracing do
  let(:tracer) { Object.new }

  def stub_open_telemetry_classes(provider:, exporter: nil, processor: nil)
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
    otlp_exporter_class = Class.new do
      define_singleton_method(:new) { |endpoint:| exporter || endpoint }
    end

    export_module.const_set(:SimpleSpanProcessor, simple_span_processor_class)
    trace_module.const_set(:TracerProvider, tracer_provider_class)
    trace_module.const_set(:Export, export_module)
    sdk_module.const_set(:Trace, trace_module)
    otlp_module.const_set(:Exporter, otlp_exporter_class)
    exporter_module.const_set(:OTLP, otlp_module)
    open_telemetry_module.const_set(:SDK, sdk_module)
    open_telemetry_module.const_set(:Exporter, exporter_module)

    stub_const('OpenTelemetry', open_telemetry_module)
  end

  before do
    described_class::TRACE_MUTEX.synchronize do
      described_class::TRACE_STATE.enabled = false
      described_class::TRACE_STATE.available = false
      described_class::TRACE_STATE.tracer = nil
      described_class::TRACE_STATE.warning_emitted = false
    end
    allow(described_class).to receive(:require).and_call_original
    allow(Kernel).to receive(:require).and_call_original
    allow(described_class).to receive(:__native_set_tracing_status__)
    allow(described_class).to receive(:__native_set_lifecycle_callback__)
    allow(described_class).to receive(:warn)
  end

  after do
    described_class.send(:write_trace_state, enabled: false, available: false, tracer: nil)
    described_class::TRACE_MUTEX.synchronize do
      described_class::TRACE_STATE.warning_emitted = false
    end
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
    allow(described_class).to receive(:build_tracer).and_return(tracer)

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
    described_class.send(:write_trace_state, enabled: true, available: true, tracer: tracer)
    allow(tracer).to receive(:in_span).and_yield

    described_class.with_request_span('REQUEST_METHOD' => 'POST', 'PATH_INFO' => '/widgets') { :ok }

    expect(tracer).to have_received(:in_span).with(
      'vajra.request',
      attributes: {
        'vajra.request.method' => 'POST',
        'vajra.request.path' => '/widgets'
      }
    )
  end

  it 'emits lifecycle spans when tracing is available' do
    described_class.send(:write_trace_state, enabled: true, available: true, tracer: tracer)
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

  it 'builds a tracer without an exporter when no endpoint is configured' do
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

    expect(described_class.send(:build_tracer, '', 'vajra-test')).to eq(tracer)
    expect(provider.processors).to be_empty
  end

  it 'builds a tracer with an OTLP exporter when an endpoint is configured' do
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

    expect(
      described_class.send(:build_tracer, 'http://127.0.0.1:4318/v1/traces', 'vajra-test')
    ).to eq(tracer)
    expect(provider.processors).to eq([processor])
  end

  it 'returns nil when the OTLP exporter dependency is unavailable' do
    provider_class = Class.new do
      def tracer(_name)
        raise 'should not be reached'
      end
    end
    provider = provider_class.new
    stub_open_telemetry_classes(provider:)
    allow(described_class).to receive(:require).with('opentelemetry/sdk').and_return(true)
    allow(described_class).to receive(:require).with('opentelemetry/exporter/otlp').and_raise(LoadError)
    allow(Kernel).to receive(:require).with('opentelemetry/sdk').and_return(true)
    allow(Kernel).to receive(:require).with('opentelemetry/exporter/otlp').and_raise(LoadError)

    expect(
      described_class.send(:build_tracer, 'http://127.0.0.1:4318/v1/traces', 'vajra-test')
    ).to be_nil
    expect(described_class).to have_received(:warn).with(include('OpenTelemetry tracing requested'))
  end

  it 'only warns once for missing tracing dependencies' do
    described_class.send(:emit_missing_dependency_warning)
    described_class.send(:emit_missing_dependency_warning)

    expect(described_class).to have_received(:warn).once
  end
end
