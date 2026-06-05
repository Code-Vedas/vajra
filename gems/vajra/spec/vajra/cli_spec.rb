# frozen_string_literal: true

# Copyright Codevedas Inc. 2025-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

require 'fileutils'
require 'tmpdir'
require_relative '../support/documented_server_options'

RSpec.describe Vajra::CLI do
  describe '.with_config_target' do
    it 'restores the previous config target after yielding' do
      Thread.current[:vajra_config_target] = :previous_target

      described_class.with_config_target(:next_target) do
        expect(described_class.current_config_target).to eq(:next_target)
      end

      expect(described_class.current_config_target).to eq(:previous_target)
    ensure
      Thread.current[:vajra_config_target] = nil
    end

    it 'skips target restoration when the config thread is unavailable' do
      allow(described_class).to receive(:config_target_thread).and_return(nil)

      expect do
        described_class.with_config_target(:next_target) { nil }
      end.not_to raise_error
    end
  end

  describe '.start' do
    it 'loads the default config file and passes configured start options' do
      Dir.mktmpdir('vajra-cli-config') do |root|
        config_dir = File.join(root, 'config')
        FileUtils.mkdir_p(config_dir)
        File.write(File.join(config_dir, 'vajra.rb'), <<~RUBY)
          Vajra.configure do |config|
            config.host "127.0.0.1"
            config.port 4321
            config.max_request_head_bytes 2048
            config.app ->(_env) { [200, { "Content-Type" => "text/plain" }, ["OK"]] }
          end
        RUBY

        allow(Vajra::Internal::RackExecution).to receive(:install!)
        allow(Vajra).to receive(:header).and_return('header')
        allow(Vajra).to receive(:start)

        described_class.start(argv: [], root:, stdout: StringIO.new)

        expect(Vajra::Internal::RackExecution).to have_received(:install!)
        expect(Vajra).to have_received(:start).with(
          host: '127.0.0.1',
          port: 4321,
          max_request_head_bytes: 2048
        )
      end
    end

    it 'accepts and propagates the native-backed server configuration surface' do
      Dir.mktmpdir('vajra-cli-config') do |root|
        config_dir = File.join(root, 'config')
        FileUtils.mkdir_p(config_dir)
        File.write(File.join(config_dir, 'vajra.rb'), DocumentedServerOptions.native_config_file_contents)

        allow(Vajra::Internal::RackExecution).to receive(:install!)
        allow(Vajra).to receive(:header).and_return('header')
        allow(Vajra).to receive(:start)

        described_class.start(argv: [], root:, stdout: StringIO.new)

        expect(Vajra::Internal::RackExecution).to have_received(:install!)
        expect(Vajra).to have_received(:start).with(DocumentedServerOptions.native_start_options)
      end
    end

    it 'rejects documented but unimplemented directives during startup' do
      Dir.mktmpdir('vajra-cli-config') do |root|
        config_dir = File.join(root, 'config')
        FileUtils.mkdir_p(config_dir)
        File.write(File.join(config_dir, 'vajra.rb'), <<~RUBY)
          Vajra.configure do |config|
            config.tls
          end
        RUBY

        allow(Vajra).to receive(:header).and_return('header')

        expect do
          described_class.start(argv: [], root:, stdout: StringIO.new)
        end.to raise_error(Vajra::Error, /start option not implemented yet: tls/)
      end
    end

    it 'loads an explicit config file path via -C' do
      Dir.mktmpdir('vajra-cli-config') do |root|
        custom_path = File.join(root, 'vajra-custom.rb')
        File.write(custom_path, <<~RUBY)
          Vajra.configure do
            port 5555
          end
        RUBY

        allow(Vajra).to receive(:header).and_return('header')
        allow(Vajra).to receive(:start)

        described_class.start(argv: ['-C', custom_path], root:, stdout: StringIO.new)

        expect(Vajra).to have_received(:start).with(port: 5555)
      end
    end

    it 'rejects multiple values for scalar directives' do
      Dir.mktmpdir('vajra-cli-config') do |root|
        config_dir = File.join(root, 'config')
        FileUtils.mkdir_p(config_dir)
        File.write(File.join(config_dir, 'vajra.rb'), <<~RUBY)
          Vajra.configure do |config|
            config.host "127.0.0.1", "0.0.0.0"
          end
        RUBY

        allow(Vajra).to receive(:header).and_return('header')

        expect do
          described_class.start(argv: [], root:, stdout: StringIO.new)
        end.to raise_error(Vajra::CLI::Error, /host expects a single value/)
      end
    end

    it 'rejects unknown configuration directives with an actionable error' do
      Dir.mktmpdir('vajra-cli-config') do |root|
        config_dir = File.join(root, 'config')
        FileUtils.mkdir_p(config_dir)
        File.write(File.join(config_dir, 'vajra.rb'), <<~RUBY)
          Vajra.configure do |config|
            config.not_a_real_setting 123
          end
        RUBY

        allow(Vajra).to receive(:header).and_return('header')

        expect do
          described_class.start(argv: [], root:, stdout: StringIO.new)
        end.to raise_error(Vajra::CLI::Error, /unsupported configuration directive: not_a_real_setting/)
      end
    end

    it 'falls back to config.ru when no config/vajra.rb is present' do
      Dir.mktmpdir('vajra-cli-rackup') do |root|
        File.write(File.join(root, 'config.ru'), "run ->(_env) { [200, { \"content-type\" => \"text/plain\" }, [\"ok\"]] }\n")

        allow(Vajra::Internal::RackExecution).to receive(:install!)
        allow(Vajra).to receive(:header).and_return('header')
        allow(Vajra).to receive(:start)

        described_class.start(argv: [], root:, stdout: StringIO.new)

        expect(Vajra::Internal::RackExecution).to have_received(:install!)
        expect(Vajra).to have_received(:start).with(no_args)
      end
    end

    it 'uses the rails directive to load config/environment and install Rails.application' do
      Dir.mktmpdir('vajra-cli-rails') do |root|
        config_dir = File.join(root, 'config')
        FileUtils.mkdir_p(config_dir)
        File.write(File.join(config_dir, 'environment.rb'), <<~RUBY)
          module Rails
            class << self
              attr_accessor :application
            end
          end

          application = Object.new
          def application.call(_env)
            [200, { "Content-Type" => "text/plain" }, ["OK"]]
          end

          def application.initialized?
            true
          end

          def application.initialize!
            self
          end

          Rails.application = application
        RUBY
        File.write(File.join(config_dir, 'vajra.rb'), <<~RUBY)
          Vajra.configure do |config|
            config.rails
          end
        RUBY

        allow(Vajra::Rails).to receive(:install!)
        allow(Vajra).to receive(:header).and_return('header')
        allow(Vajra).to receive(:start)

        described_class.start(argv: [], root:, stdout: StringIO.new)

        expect(Vajra::Rails).to have_received(:install!)
        expect(Vajra).to have_received(:start).with(no_args)
      end
    end

    it 'rejects unknown arguments' do
      expect do
        described_class.start(argv: ['--unknown'], root: Dir.pwd, stdout: StringIO.new)
      end.to raise_error(Vajra::CLI::Error, /invalid option: --unknown/)
    end

    it 'rejects stray positional arguments' do
      expect do
        described_class.start(argv: ['config.ru'], root: Dir.pwd, stdout: StringIO.new)
      end.to raise_error(Vajra::CLI::Error, /unknown arguments: config\.ru/)
    end

    it 'wraps config file load errors' do
      Dir.mktmpdir('vajra-cli-bad-config') do |root|
        config_dir = File.join(root, 'config')
        FileUtils.mkdir_p(config_dir)
        File.write(File.join(config_dir, 'vajra.rb'), "raise 'boom'\n")

        allow(Vajra).to receive(:header).and_return('header')

        expect do
          described_class.start(argv: [], root:, stdout: StringIO.new)
        end.to raise_error(Vajra::CLI::Error, /unable to load .*boom/)
      end
    end

    it 'wraps missing explicit config files' do
      allow(Vajra).to receive(:header).and_return('header')

      expect do
        described_class.start(argv: ['-C', 'missing/vajra.rb'], root: Dir.pwd, stdout: StringIO.new)
      end.to raise_error(Vajra::CLI::Error, %r{config file not found: .*missing/vajra\.rb})
    end

    it 'installs an app declared with the config block form' do
      Dir.mktmpdir('vajra-cli-config') do |root|
        config_dir = File.join(root, 'config')
        FileUtils.mkdir_p(config_dir)
        File.write(File.join(config_dir, 'vajra.rb'), <<~RUBY)
          Vajra.configure do |config|
            config.app do
              ->(_env) { [200, { "Content-Type" => "text/plain" }, ["OK"]] }
            end
          end
        RUBY

        allow(Vajra::Internal::RackExecution).to receive(:install!)
        allow(Vajra).to receive(:header).and_return('header')
        allow(Vajra).to receive(:start)

        described_class.start(argv: [], root:, stdout: StringIO.new)

        expect(Vajra::Internal::RackExecution).to have_received(:install!)
        expect(Vajra).to have_received(:start).with(no_args)
      end
    end

    it 'rejects the app directive when it is called without an argument or block' do
      Dir.mktmpdir('vajra-cli-config') do |root|
        config_dir = File.join(root, 'config')
        FileUtils.mkdir_p(config_dir)
        File.write(File.join(config_dir, 'vajra.rb'), <<~RUBY)
          Vajra.configure do |config|
            config.app
          end
        RUBY

        allow(Vajra).to receive(:header).and_return('header')

        expect do
          described_class.start(argv: [], root:, stdout: StringIO.new)
        end.to raise_error(Vajra::CLI::Error, /app requires either a Rack app argument or a block/)
      end
    end

    it 'rejects the app directive when it is called with more than one argument' do
      Dir.mktmpdir('vajra-cli-config') do |root|
        config_dir = File.join(root, 'config')
        FileUtils.mkdir_p(config_dir)
        File.write(File.join(config_dir, 'vajra.rb'), <<~RUBY)
          Vajra.configure do |config|
            config.app ->(_env) { [200, {}, []] }, ->(_env) { [200, {}, []] }
          end
        RUBY

        allow(Vajra).to receive(:header).and_return('header')

        expect do
          described_class.start(argv: [], root:, stdout: StringIO.new)
        end.to raise_error(Vajra::CLI::Error, /app expects at most one Rack app argument/)
      end
    end

    it 'raises outside configuration loading for Vajra.configure' do
      expect do
        Vajra.configure { |config| config.port 3000 }
      end.to raise_error(Vajra::Error, /only available while loading Vajra configuration/)
    end
  end

  describe Vajra::CLI::Launcher do
    subject(:launcher) { described_class.new(root: Dir.pwd) }

    it 'keeps documented server directives unique' do
      expect(Vajra::CLI::DOCUMENTED_SERVER_SETTINGS).to eq(Vajra::CLI::DOCUMENTED_SERVER_SETTINGS.uniq)
    end

    it 'allows explicit values for boolean directives' do
      expect(launcher.send(:normalize_setting_values, :structured_logs, [false])).to be(false)
    end

    it 'normalizes nil access_log to the disabled default' do
      expect(launcher.send(:normalize_setting_values, :access_log, [nil])).to be_nil
    end

    it 'reports supported and unsupported directives through respond_to_missing?' do
      expect(launcher.send(:respond_to_missing?, :port)).to be(true)
      expect(launcher.send(:respond_to_missing?, :unsupported_directive)).to be(false)
    end

    it 'normalizes thread settings to two integers' do
      expect(launcher.send(:normalize_setting_values, :threads, %w[5 8])).to eq([5, 8])
    end

    it 'normalizes a single thread value to fixed min and max threads' do
      expect(launcher.send(:normalize_setting_values, :threads, ['5'])).to eq([5, 5])
    end

    it 'normalizes array-backed thread settings passed as one array value' do
      expect(launcher.send(:normalize_setting_values, :threads, [[5, 8]])).to eq([5, 8])
    end

    it 'rejects invalid thread setting shapes' do
      expect do
        launcher.send(:normalize_setting_values, :threads, [1, 2, 3])
      end.to raise_error(Vajra::CLI::Error, 'threads expects one or two integer values')
    end

    it 'normalizes alpn_protocols to strings' do
      expect(launcher.send(:normalize_setting_values, :alpn_protocols, [:h2, 'http/1.1']))
        .to eq(%w[h2 http/1.1])
    end

    it 'rejects empty alpn_protocols values' do
      expect do
        launcher.send(:normalize_setting_values, :alpn_protocols, [])
      end.to raise_error(Vajra::CLI::Error, 'alpn_protocols expects at least one value')
    end

    it 'passes through unsupported array directives unchanged in the private normalizer' do
      expect(launcher.send(:normalize_array_setting, :unsupported_array_directive, ['value'])).to eq(['value'])
    end
  end
end
