# frozen_string_literal: true

# Copyright Codevedas Inc. 2025-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

require 'fileutils'
require 'tmpdir'
require_relative '../support/documented_server_options'

RSpec.describe Vajra::CLI do
  describe '.start!' do
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

    it 'accepts and propagates the full documented server configuration surface' do
      Dir.mktmpdir('vajra-cli-config') do |root|
        config_dir = File.join(root, 'config')
        FileUtils.mkdir_p(config_dir)
        File.write(File.join(config_dir, 'vajra.rb'), DocumentedServerOptions.config_file_contents)

        allow(Vajra::Internal::RackExecution).to receive(:install!)
        allow(Vajra).to receive(:header).and_return('header')
        allow(Vajra).to receive(:start)

        described_class.start(argv: [], root:, stdout: StringIO.new)

        expect(Vajra::Internal::RackExecution).to have_received(:install!)
        expect(Vajra).to have_received(:start).with(DocumentedServerOptions.start_options)
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

    it 'rejects unsupported directives during normalization' do
      expect do
        launcher.send(:normalize_setting_values, :unsupported_directive, ['value'])
      end.to raise_error(Vajra::CLI::Error, 'unsupported configuration directive: unsupported_directive')
    end
  end
end
