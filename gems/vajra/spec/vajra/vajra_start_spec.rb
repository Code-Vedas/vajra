# frozen_string_literal: true

# Copyright Codevedas Inc. 2025-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

require_relative '../support/documented_server_options'

RSpec.describe Vajra, '.start' do
  it 'keeps the native start entrypoint public' do
    expect(described_class).to respond_to(:start)
    expect(described_class.singleton_methods).not_to include(:native_start)
  end

  describe '.configure' do
    let(:config_target) { Class.new.new }

    it 'yields the current config target when the block expects an argument' do
      yielded_target = nil

      Vajra::CLI.with_config_target(config_target) do
        described_class.configure { |config| yielded_target = config }
      end

      expect(yielded_target).to equal(config_target)
    end

    it 'instance-evals the current config target when the block takes no arguments' do
      config_target.define_singleton_method(:configured?) { true }
      configured = nil

      Vajra::CLI.with_config_target(config_target) do
        described_class.configure { configured = configured? }
      end

      expect(configured).to be(true)
    end

    it 'raises outside Vajra configuration loading' do
      expect { described_class.configure { nil } }
        .to raise_error(Vajra::Error, 'Vajra.configure is only available while loading Vajra configuration')
    end
  end

  describe '.start' do
    it 'validates the full documented start surface and forwards native-backed options only' do
      allow(described_class).to receive(:__native_start__)

      described_class.start(**DocumentedServerOptions.start_options)

      expect(described_class).to have_received(:__native_start__).with(
        host: '127.0.0.1',
        port: 4321,
        workers: 4,
        threads: [5, 5],
        max_connections: 10_000,
        queue_capacity: 5000,
        scheduler_policy: 'least_loaded',
        max_request_head_bytes: 2048,
        request_timeout: 25,
        request_head_timeout: 15,
        first_data_timeout: 30,
        persistent_timeout: 30,
        worker_timeout: 60,
        log_level: 'info'
      )
    end

    it 'rejects unknown start options' do
      expect { described_class.start(unknown_option: 1) }
        .to raise_error(Vajra::Error, 'unknown start option: unknown_option')
    end
  end

  describe '.stop' do
    it 'delegates to the native stop entrypoint' do
      allow(described_class).to receive(:__native_stop__)

      described_class.stop

      expect(described_class).to have_received(:__native_stop__)
    end
  end
end
