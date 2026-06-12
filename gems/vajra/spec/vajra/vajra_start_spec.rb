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

    it 'installs the optional Railtie when Rails::Railtie is defined' do
      stub_const('Rails', Module.new)
      stub_const('Rails::Railtie', Class.new)
      allow(described_class).to receive(:require_relative).with('vajra/railtie')

      described_class.install_optional_railtie

      expect(described_class).to have_received(:require_relative).with('vajra/railtie')
    end

    it 'skips optional Railtie installation when Rails::Railtie is absent' do
      hide_const('Rails')
      allow(described_class).to receive(:require_relative)

      described_class.install_optional_railtie

      expect(described_class).not_to have_received(:require_relative)
    end

    it 'suppresses optional Railtie load failures' do
      stub_const('Rails', Module.new)
      stub_const('Rails::Railtie', Class.new)
      allow(described_class).to receive(:require_relative).with('vajra/railtie').and_raise(LoadError)

      expect { described_class.install_optional_railtie }.not_to raise_error
    end

    it 'does not suppress railtie NoMethodError failures' do
      stub_const('Rails', Module.new)
      stub_const('Rails::Railtie', Class.new)
      allow(described_class).to receive(:require_relative).with('vajra/railtie').and_raise(NoMethodError, 'boom')

      expect { described_class.install_optional_railtie }.to raise_error(NoMethodError, /boom/)
    end

    it 'yields the current config target when the block uses an optional argument' do
      yielded_target = nil

      Vajra::CLI.with_config_target(config_target) do
        described_class.configure { |config = nil| yielded_target = config }
      end

      expect(yielded_target).to equal(config_target)
    end

    it 'yields the current config target when the block uses a rest argument' do
      yielded_target = nil

      Vajra::CLI.with_config_target(config_target) do
        described_class.configure { |*config| yielded_target = config.fetch(0) }
      end

      expect(yielded_target).to equal(config_target)
    end

    it 'raises outside Vajra configuration loading' do
      expect { described_class.configure { nil } }
        .to raise_error(Vajra::Error, 'Vajra.configure is only available while loading Vajra configuration')
    end

    it 'raises a clear error when called without a block' do
      Vajra::CLI.with_config_target(config_target) do
        expect { described_class.configure }
          .to raise_error(Vajra::Error, 'Vajra.configure requires a block')
      end
    end
  end

  describe '.start' do
    it 'accepts the native-backed start surface' do
      allow(described_class).to receive(:__native_start__)
      allow(Vajra::Internal::RackExecution).to receive(:configure_threads!)

      described_class.start(**DocumentedServerOptions.native_start_options)

      expect(described_class).to have_received(:__native_start__).with(DocumentedServerOptions.native_start_options)
      expect(Vajra::Internal::RackExecution).to have_received(:configure_threads!).with(5)
    end

    it 'uses the native default single-thread Rack env truth when threads are omitted' do
      allow(described_class).to receive(:__native_start__)
      allow(Vajra::Internal::RackExecution).to receive(:configure_threads!)

      described_class.start

      expect(described_class).to have_received(:__native_start__).with(no_args)
      expect(Vajra::Internal::RackExecution).to have_received(:configure_threads!).with(1)
    end

    it 'accepts TLS and HTTP/2 start options as native-backed settings' do
      allow(described_class).to receive(:__native_start__)
      options = DocumentedServerOptions.native_start_options.merge(
        tls: true,
        tls_certificate: 'config/certs/server.crt',
        tls_private_key: 'config/certs/server.key',
        tls_ca_certificate: 'config/certs/ca.crt',
        tls_verify_mode: 'peer',
        tls_min_version: 'TLSv1_2',
        alpn_protocols: %w[h2 http/1.1],
        http2: true,
        http2_max_concurrent_streams: 128,
        http2_initial_window_size: 1_048_576,
        http2_max_frame_size: 1_048_576,
        http2_header_table_size: 4096
      )

      described_class.start(**options)

      expect(described_class).to have_received(:__native_start__).with(options)
    end

    it 'rejects unknown start options' do
      expect { described_class.start(unknown_option: 1) }
        .to raise_error(Vajra::Error, 'unknown start option: unknown_option')
    end

    it 'rejects unsupported start options as unknown' do
      expect { described_class.start(bind: 'tcp://127.0.0.1:3000') }
        .to raise_error(Vajra::Error, 'unknown start option: bind')
    end

    it 'rejects invalid boolean start option values before native start' do
      expect { described_class.start(tls: 'true') }
        .to raise_error(Vajra::Error, 'Unable to start Vajra: tls option must be true or false')
    end

    it 'rejects nil boolean start option values before native start' do
      expect { described_class.start(tls: nil) }
        .to raise_error(Vajra::Error, 'Unable to start Vajra: tls option must be true or false')
    end

    it 'rejects invalid string start option values before native start' do
      expect { described_class.start(tls_certificate: :server_cert) }
        .to raise_error(Vajra::Error, 'Unable to start Vajra: tls_certificate option must be a String')
    end

    it 'rejects nil string start option values except nil access_log' do
      expect { described_class.start(tls_certificate: nil) }
        .to raise_error(Vajra::Error, 'Unable to start Vajra: tls_certificate option must be a String')
    end

    it 'accepts nil access_log as the disabled access log setting' do
      allow(described_class).to receive(:__native_start__)

      described_class.start(access_log: nil)

      expect(described_class).to have_received(:__native_start__).with(access_log: nil)
    end

    it 'accepts request_body_timeout as a native-backed start setting' do
      allow(described_class).to receive(:__native_start__)

      described_class.start(request_body_timeout: 1)

      expect(described_class).to have_received(:__native_start__).with(request_body_timeout: 1)
    end

    it 'rejects invalid ALPN values before native start' do
      expect { described_class.start(alpn_protocols: 'h2') }
        .to raise_error(Vajra::Error, 'Unable to start Vajra: alpn_protocols option must be an Array of Strings')

      expect { described_class.start(alpn_protocols: []) }
        .to raise_error(Vajra::Error, 'Unable to start Vajra: alpn_protocols option must not be empty')

      expect { described_class.start(alpn_protocols: [:h2]) }
        .to raise_error(Vajra::Error, 'Unable to start Vajra: alpn_protocols option must be an Array of Strings')

      expect { described_class.start(alpn_protocols: ['']) }
        .to raise_error(Vajra::Error, 'Unable to start Vajra: alpn_protocols option values must not be empty')
    end

    it 'rejects invalid integer start option values before native start' do
      expect { described_class.start(max_request_body_bytes: '1048576') }
        .to raise_error(Vajra::Error, 'Unable to start Vajra: invalid max_request_body_bytes option: 1048576. Expected an integer between 1 and 2147483647')

      expect { described_class.start(http2_max_frame_size: 16_383) }
        .to raise_error(Vajra::Error, 'Unable to start Vajra: invalid http2_max_frame_size option: 16383. Expected an integer between 16384 and 16777215')

      expect { described_class.start(port: 65_536) }
        .to raise_error(Vajra::Error, 'Unable to start Vajra: invalid port option: 65536. Expected an integer between 0 and 65535')
    end

    it 'rejects invalid thread ranges before native start' do
      [
        ['5', 'threads option must be an Array of two Integers'],
        [[5], 'threads option must be an Array of two Integers'],
        [[1, 2, 3], 'threads option must be an Array of two Integers'],
        [[1, '2'], 'threads option must be an Array of two Integers'],
        [[0, 1], 'invalid threads option: expected thread range with 1 <= min <= max'],
        [[2, 1], 'invalid threads option: expected thread range with 1 <= min <= max']
      ].each do |value, message|
        expect { described_class.start(threads: value) }
          .to raise_error(Vajra::Error, "Unable to start Vajra: #{message}")
      end
    end

    it 'rejects invalid TLS and HTTP/2 protocol combinations before native start' do
      expect { described_class.start(tls_verify_mode: 'required') }
        .to raise_error(Vajra::Error, 'Unable to start Vajra: tls_verify_mode option must be none or peer')

      expect { described_class.start(tls_min_version: 'TLSv1_1') }
        .to raise_error(Vajra::Error, 'Unable to start Vajra: tls_min_version option must be TLSv1_2 or TLSv1_3')

      expect { described_class.start(tls: true) }
        .to raise_error(Vajra::Error, 'Unable to start Vajra: tls requires tls_certificate and tls_private_key')

      expect { described_class.start(alpn_protocols: %w[h2 http/1.1]) }
        .to raise_error(Vajra::Error, 'Unable to start Vajra: alpn_protocols cannot include h2 unless http2 is enabled')
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
