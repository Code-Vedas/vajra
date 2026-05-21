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

      described_class.start(**DocumentedServerOptions.native_start_options)

      expect(described_class).to have_received(:__native_start__).with(DocumentedServerOptions.native_start_options)
    end

    it 'rejects unknown start options' do
      expect { described_class.start(unknown_option: 1) }
        .to raise_error(Vajra::Error, 'unknown start option: unknown_option')
    end

    it 'rejects documented start options that are not implemented yet' do
      expect { described_class.start(tls: true) }
        .to raise_error(Vajra::Error, 'start option not implemented yet: tls')
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
