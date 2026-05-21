# frozen_string_literal: true

# Copyright Codevedas Inc. 2025-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

require 'fileutils'
require 'rails'
require 'rails/railtie'
require 'tmpdir'
require 'rackup/handler/vajra'
require 'vajra/railtie'
require_relative '../support/documented_server_options'

RSpec.describe 'Vajra Rails integration', :aggregate_failures do # rubocop:disable RSpec/DescribeClass
  let(:rack_app) { ->(_env) { [200, { 'Content-Type' => 'text/plain' }, ['OK']] } }

  around do |example|
    original_handler = ENV.fetch('RACKUP_HANDLER', nil)
    ENV.delete('RACKUP_HANDLER')
    example.run
  ensure
    ENV['RACKUP_HANDLER'] = original_handler
  end

  before do
    Vajra::Internal::RackExecution.uninstall!
    allow(Vajra).to receive(:start)
    allow(Vajra).to receive(:stop)
  end

  after do
    Vajra::Internal::RackExecution.uninstall!
  end

  it 'defaults rails server selection to vajra' do
    callback, = Vajra::Railtie.config.before_configuration.first
    callback.call

    expect(ENV.fetch('RACKUP_HANDLER', nil)).to eq('vajra')
  end

  it 'does not overwrite an explicit rackup handler selection' do
    ENV['RACKUP_HANDLER'] = 'falcon'

    callback, = Vajra::Railtie.config.before_configuration.first
    callback.call

    expect(ENV.fetch('RACKUP_HANDLER')).to eq('falcon')
  end

  it 'registers itself as a rackup handler' do
    expect(Rackup::Handler.get(:vajra)).to eq(Rackup::Handler::Vajra)
  end

  it 'registers itself through the rack compatibility shim' do
    require 'rack/handler/vajra'

    expect(Rackup::Handler.get(:vajra)).to eq(Rackup::Handler::Vajra)
  end

  it 'exposes rackup handler option help' do
    expect(Rackup::Handler::Vajra.valid_options).to include(
      'Host=HOST' => a_string_including('Bind address to listen on'),
      'Port=PORT' => a_string_including('Port to listen on'),
      'max_request_head_bytes=BYTES' => a_string_including('Maximum allowed HTTP request head size')
    )
  end

  it 'installs the rack app and starts Vajra with rackup options' do
    Dir.mktmpdir('vajra-handler-root') do |root|
      Dir.chdir(root) do # rubocop:disable ThreadSafety/DirChdir
        Rackup::Handler::Vajra.run(rack_app, Host: '127.0.0.1', Port: 4321, user_supplied_options: %i[Host Port])
      end
    end

    expect(Vajra::Internal::RackExecution.installed?).to be(true)
    expect(Vajra).to have_received(:start).with(host: '127.0.0.1', port: 4321)
  end

  it 'loads max_request_head_bytes from config/vajra.rb' do
    Dir.mktmpdir('vajra-handler-root') do |root|
      Dir.chdir(root) do # rubocop:disable ThreadSafety/DirChdir
        FileUtils.mkdir_p('config')
        File.write('config/vajra.rb', <<~RUBY)
          Vajra.configure do
            max_request_head_bytes 32768
          end
        RUBY

        Rackup::Handler::Vajra.run(rack_app, Port: 3000, user_supplied_options: [])
      end
    end

    expect(Vajra).to have_received(:start).with(port: 3000, max_request_head_bytes: 32_768)
  end

  it 'accepts the native-backed config surface while keeping the current handler start surface' do
    Dir.mktmpdir('vajra-handler-root') do |root|
      Dir.chdir(root) do # rubocop:disable ThreadSafety/DirChdir
        FileUtils.mkdir_p('config')
        File.write('config/vajra.rb', DocumentedServerOptions.native_config_file_contents)

        Rackup::Handler::Vajra.run(rack_app, Port: 3000, user_supplied_options: [])
      end
    end

    expect(Vajra).to have_received(:start).with(DocumentedServerOptions.native_start_options)
  end

  it 'prefers an explicit handler max_request_head_bytes option over config defaults' do
    Dir.mktmpdir('vajra-handler-root') do |root|
      Dir.chdir(root) do # rubocop:disable ThreadSafety/DirChdir
        FileUtils.mkdir_p('config')
        File.write('config/vajra.rb', <<~RUBY)
          Vajra.configure do
            max_request_head_bytes 32768
          end
        RUBY

        Rackup::Handler::Vajra.run(
          rack_app,
          Port: 3000,
          max_request_head_bytes: 4096,
          user_supplied_options: []
        )
      end
    end

    expect(Vajra).to have_received(:start).with(port: 3000, max_request_head_bytes: 4096)
  end

  it 'keeps the configured port when rails did not receive an explicit port flag' do
    Dir.mktmpdir('vajra-handler-root') do |root|
      Dir.chdir(root) do # rubocop:disable ThreadSafety/DirChdir
        FileUtils.mkdir_p('config')
        File.write('config/vajra.rb', <<~RUBY)
          Vajra.configure do
            port 3456
          end
        RUBY

        Rackup::Handler::Vajra.run(rack_app, Port: 3000, user_supplied_options: [])
      end
    end

    expect(Vajra).to have_received(:start).with(port: 3456)
  end

  it 'keeps the configured host when rails did not receive an explicit host flag' do
    Dir.mktmpdir('vajra-handler-root') do |root|
      Dir.chdir(root) do # rubocop:disable ThreadSafety/DirChdir
        FileUtils.mkdir_p('config')
        File.write('config/vajra.rb', <<~RUBY)
          Vajra.configure do
            host "127.0.0.1"
          end
        RUBY

        Rackup::Handler::Vajra.run(rack_app, Port: 3000, user_supplied_options: [])
      end
    end

    expect(Vajra).to have_received(:start).with(host: '127.0.0.1', port: 3000)
  end

  it 'supports shutdown through Vajra.stop' do
    Rackup::Handler::Vajra.shutdown

    expect(Vajra).to have_received(:stop)
  end
end
