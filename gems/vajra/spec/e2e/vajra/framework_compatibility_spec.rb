# frozen_string_literal: true

# Copyright Codevedas Inc. 2025-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

require 'json'
require_relative 'support'

RSpec.describe 'Vajra framework compatibility', :e2e, :integration do
  def framework_request(trace_id)
    "POST /framework HTTP/1.1\r\n" \
      "Host: example.test\r\n" \
      "Content-Type: text/plain\r\n" \
      "Content-Length: 7\r\n" \
      "X-Trace-Id: #{trace_id}\r\n" \
      "Connection: close\r\n\r\n" \
      'payload'
  end

  def rails_server_files
    {
      'bin/rails' => <<~RUBY,
        #!/usr/bin/env ruby
        APP_PATH = File.expand_path("../config/application", __dir__)
        require_relative "../config/boot"
        require "rails/commands"
      RUBY
      'config/boot.rb' => <<~RUBY,
        require "bundler/setup"
      RUBY
      'config/application.rb' => <<~RUBY,
        require_relative "boot"
        require "vajra"
        require "rails"
        require "action_controller/railtie"

        class FrameworkCompatibilityRailsServerApp < ::Rails::Application
          config.root = File.expand_path("..", __dir__)
          config.eager_load = false
          config.secret_key_base = "framework-compatibility-secret-key-base"
          config.hosts << "example.test"
          config.consider_all_requests_local = false
        end

        class FrameworkCompatibilityRailsServerController < ActionController::Base
          protect_from_forgery with: :null_session

          def create
            render json: {
              framework: "rails",
              launcher: "rails_server",
              request_method: request.request_method,
              path_info: request.path_info,
              trace_id: request.headers["X-Trace-Id"],
              request_body: request.raw_post
            }
          end
        end

        FrameworkCompatibilityRailsServerApp.routes.append do
          post "/framework", to: "framework_compatibility_rails_server#create"
        end
      RUBY
      'config/environment.rb' => <<~RUBY,
        require_relative "application"
        Rails.application.initialize!
      RUBY
      'config.ru' => <<~RUBY,
        require_relative "config/environment"
        run Rails.application
      RUBY
      'config/vajra.rb' => <<~RUBY
        Vajra.configure do |config|
          config.max_request_head_bytes 32768
        end
      RUBY
    }
  end

  def hanami_files
    {
      'config/app.rb' => <<~RUBY,
        require "hanami"
        require "stringio"

        module FrameworkCompatibilityHanami
          class App < Hanami::App
            config.logger.stream = StringIO.new
          end
        end
      RUBY
      'app/action.rb' => <<~RUBY,
        require "hanami/action"

        module FrameworkCompatibilityHanami
          class Action < Hanami::Action
          end
        end
      RUBY
      'config/routes.rb' => <<~RUBY,
        require "hanami/routes"

        module FrameworkCompatibilityHanami
          class Routes < Hanami::Routes
            post "/framework", to: "framework.create"
          end
        end
      RUBY
      'app/actions/framework/create.rb' => <<~RUBY,
        require "json"

        module FrameworkCompatibilityHanami
          module Actions
            module Framework
              class Create < FrameworkCompatibilityHanami::Action
                def handle(req, res)
                  res.format = :json
                  res.body = JSON.generate(
                    framework: "hanami",
                    request_method: req.request_method,
                    path_info: req.path_info,
                    trace_id: req.get_header("HTTP_X_TRACE_ID"),
                    request_body: req.body.read
                  )
                end
              end
            end
          end
        end
      RUBY
      'config.ru' => <<~RUBY
        require "hanami/boot"
        run Hanami.app
      RUBY
    }
  end

  it 'serves a Rails app from config/vajra.rb with no manual boot script' do
    result = packaged_app_request_result(
      files: {
        'config/vajra.rb' => <<~RUBY,
          Vajra.configure do |config|
            config.rails
          end
        RUBY
        'config/environment.rb' => <<~RUBY
          require "json"
          require "logger"
          require "rails"
          require "action_controller/railtie"

          class FrameworkCompatibilityRailsApp < ::Rails::Application
            config.root = Dir.pwd
            config.eager_load = false
            config.secret_key_base = "framework-compatibility-secret-key-base"
            config.hosts << "example.test"
            config.logger = Logger.new($stdout)
            config.log_level = :fatal
            config.consider_all_requests_local = false
          end

          class FrameworkCompatibilityRailsController < ActionController::Base
            protect_from_forgery with: :null_session

            def create
              render json: {
                framework: "rails",
                request_method: request.request_method,
                path_info: request.path_info,
                trace_id: request.headers["X-Trace-Id"],
                request_body: request.raw_post
              }
            end
          end

          FrameworkCompatibilityRailsApp.routes.append do
            post "/framework", to: "framework_compatibility_rails#create"
          end
        RUBY
      },
      request: framework_request('rails-123')
    )

    response = parse_http_response(result[:response])
    payload = JSON.parse(response[:body])

    expect(result[:exitstatus]).to eq(0), result[:output]
    expect(response[:status_line]).to eq('HTTP/1.1 200 OK')
    expect(response[:headers]).to include(
      'content-type' => a_string_including('application/json'),
      'content-length' => response[:body].bytesize.to_s,
      'connection' => 'close'
    )
    expect(payload).to include(
      'framework' => 'rails',
      'request_method' => 'POST',
      'path_info' => '/framework',
      'trace_id' => 'rails-123',
      'request_body' => 'payload'
    )
  end

  it 'serves a Rails app from bin/rails server without requiring Puma' do
    port = begin
      server = TCPServer.new(VajraE2EHelpers::LISTENER_BIND_HOST, 0)
      server.addr[1]
    ensure
      server&.close
    end

    result = packaged_app_command_request_result_on_port(
      files: rails_server_files,
      command: packaged_bundle_command(RbConfig.ruby, 'bin/rails', 'server'),
      port:,
      env: { 'PORT' => port.to_s },
      request: framework_request('rails-server-123')
    )

    response = parse_http_response(result[:response])
    payload = JSON.parse(response[:body])

    expect(result[:exitstatus]).to eq(0), result[:output]
    expect(response[:status_line]).to eq('HTTP/1.1 200 OK')
    expect(payload).to include(
      'framework' => 'rails',
      'launcher' => 'rails_server',
      'request_method' => 'POST',
      'path_info' => '/framework',
      'trace_id' => 'rails-server-123',
      'request_body' => 'payload'
    )
  end

  it 'exits bin/rails server on Ctrl-C' do
    port = begin
      server = TCPServer.new(VajraE2EHelpers::LISTENER_BIND_HOST, 0)
      server.addr[1]
    ensure
      server&.close
    end

    with_packaged_app(files: rails_server_files) do |app_root|
      managed_popen2e(
        app_root_bundle_env.merge('PORT' => port.to_s),
        *packaged_bundle_command(RbConfig.ruby, 'bin/rails', 'server'),
        chdir: app_root
      ) do |_stdin, output, wait_thread|
        startup_output = []
        selected_port = wait_for_banner(output, captured_lines: startup_output)
        expect(selected_port).to eq(port)

        status = stop_process(wait_thread)
        full_output = "#{startup_output.join}#{output.read}"

        expect(status.exitstatus).to eq(0), full_output
        expect(full_output).to include('- Gracefully shutting down workers...')
        expect(full_output).to include('=== vajra shutdown:')
        expect(full_output).to include('- Goodbye!')
      ensure
        cleanup_process(wait_thread, output)
      end
    end
  end

  it 'serves a Rails app through the Rails adapter and Rack worker execution seam' do
    script = <<~RUBY
      require "json"
      require "logger"
      require "rails"
      require "action_controller/railtie"
      require "vajra/rails"

      class FrameworkCompatibilityRailsApp < ::Rails::Application
        config.root = Dir.pwd
        config.eager_load = false
        config.secret_key_base = "framework-compatibility-secret-key-base"
        config.hosts << "example.test"
        config.logger = Logger.new($stdout)
        config.log_level = :fatal
        config.consider_all_requests_local = false
      end

      class FrameworkCompatibilityRailsController < ActionController::Base
        protect_from_forgery with: :null_session

        def create
          render json: {
            framework: "rails",
            request_method: request.request_method,
            path_info: request.path_info,
            trace_id: request.headers["X-Trace-Id"],
            request_body: request.raw_post
          }
        end
      end

      FrameworkCompatibilityRailsApp.routes.append do
        post "/framework", to: "framework_compatibility_rails#create"
      end

      Vajra::Rails.install!(FrameworkCompatibilityRailsApp)
      Vajra.start
    RUBY

    result = rack_app_request_result(
      script:,
      request:
        "POST /framework HTTP/1.1\r\n" \
        "Host: example.test\r\n" \
        "Content-Type: text/plain\r\n" \
        "Content-Length: 7\r\n" \
        "X-Trace-Id: rails-123\r\n" \
        "Connection: close\r\n\r\n" \
        'payload'
    )

    response = parse_http_response(result[:response])
    payload = JSON.parse(response[:body])

    expect(result[:exitstatus]).to eq(0), result[:output]
    expect(response[:status_line]).to eq('HTTP/1.1 200 OK')
    expect(response[:headers]).to include(
      'content-type' => a_string_including('application/json'),
      'content-length' => response[:body].bytesize.to_s,
      'connection' => 'close'
    )
    expect(payload).to include(
      'framework' => 'rails',
      'request_method' => 'POST',
      'path_info' => '/framework',
      'trace_id' => 'rails-123',
      'request_body' => 'payload'
    )
  end

  it 'serves a Sinatra app through the Rack worker execution seam' do
    script = <<~RUBY
      require "json"
      require "sinatra/base"
      require "vajra"

      class FrameworkCompatibilitySinatraApp < Sinatra::Base
        disable :logging
        disable :show_exceptions
        disable :raise_errors

        post "/framework" do
          content_type "application/json"
          JSON.generate(
            framework: "sinatra",
            request_method: request.request_method,
            path_info: request.path_info,
            trace_id: request.env["HTTP_X_TRACE_ID"],
            request_body: request.body.read
          )
        end
      end

      Vajra::Internal::RackExecution.install!(FrameworkCompatibilitySinatraApp.new)
      Vajra.start
    RUBY

    result = rack_app_request_result(
      script:,
      request: framework_request('sinatra-123')
    )

    response = parse_http_response(result[:response])
    payload = JSON.parse(response[:body])

    expect(result[:exitstatus]).to eq(0), result[:output]
    expect(response[:status_line]).to eq('HTTP/1.1 200 OK')
    expect(response[:headers]).to include(
      'content-type' => 'application/json',
      'content-length' => response[:body].bytesize.to_s,
      'connection' => 'close'
    )
    expect(payload).to include(
      'framework' => 'sinatra',
      'request_method' => 'POST',
      'path_info' => '/framework',
      'trace_id' => 'sinatra-123',
      'request_body' => 'payload'
    )
  end

  it 'serves a Sinatra app from config.ru with no custom launcher code' do
    result = packaged_app_request_result(
      files: {
        'app.rb' => <<~RUBY,
          require "json"
          require "sinatra/base"

          class FrameworkCompatibilitySinatraConfigRuApp < Sinatra::Base
            disable :logging
            disable :show_exceptions
            disable :raise_errors

            post "/framework" do
              content_type "application/json"
              JSON.generate(
                framework: "sinatra",
                request_method: request.request_method,
                path_info: request.path_info,
                trace_id: request.env["HTTP_X_TRACE_ID"],
                request_body: request.body.read
              )
            end
          end
        RUBY
        'config.ru' => <<~RUBY
          require_relative "./app"
          run FrameworkCompatibilitySinatraConfigRuApp.new
        RUBY
      },
      request: framework_request('sinatra-ru-123')
    )

    response = parse_http_response(result[:response])
    payload = JSON.parse(response[:body])

    expect(result[:exitstatus]).to eq(0), result[:output]
    expect(response[:status_line]).to eq('HTTP/1.1 200 OK')
    expect(payload).to include(
      'framework' => 'sinatra',
      'request_method' => 'POST',
      'path_info' => '/framework',
      'trace_id' => 'sinatra-ru-123',
      'request_body' => 'payload'
    )
  end

  it 'serves a Roda app through the Rack worker execution seam' do
    script = <<~RUBY
      require "json"
      require "roda"
      require "vajra"

      class FrameworkCompatibilityRodaApp < Roda
        route do |routing|
          routing.post "framework" do
            response["Content-Type"] = "application/json"
            JSON.generate(
              framework: "roda",
              request_method: request.request_method,
              path_info: request.path_info,
              trace_id: request.env["HTTP_X_TRACE_ID"],
              request_body: request.body.read
            )
          end
        end
      end

      Vajra::Internal::RackExecution.install!(FrameworkCompatibilityRodaApp.app)
      Vajra.start
    RUBY

    result = rack_app_request_result(
      script:,
      request: framework_request('roda-123')
    )

    response = parse_http_response(result[:response])
    payload = JSON.parse(response[:body])

    expect(result[:exitstatus]).to eq(0), result[:output]
    expect(response[:status_line]).to eq('HTTP/1.1 200 OK')
    expect(response[:headers]).to include(
      'content-type' => 'application/json',
      'content-length' => response[:body].bytesize.to_s,
      'connection' => 'close'
    )
    expect(payload).to include(
      'framework' => 'roda',
      'request_method' => 'POST',
      'path_info' => '/framework',
      'trace_id' => 'roda-123',
      'request_body' => 'payload'
    )
  end

  it 'serves a Roda app from config.ru with no custom launcher code' do
    result = packaged_app_request_result(
      files: {
        'app.rb' => <<~RUBY,
          require "json"
          require "roda"

          class FrameworkCompatibilityRodaConfigRuApp < Roda
            route do |routing|
              routing.post "framework" do
                response["Content-Type"] = "application/json"
                JSON.generate(
                  framework: "roda",
                  request_method: request.request_method,
                  path_info: request.path_info,
                  trace_id: request.env["HTTP_X_TRACE_ID"],
                  request_body: request.body.read
                )
              end
            end
          end
        RUBY
        'config.ru' => <<~RUBY
          require_relative "./app"
          run FrameworkCompatibilityRodaConfigRuApp.app
        RUBY
      },
      request: framework_request('roda-ru-123')
    )

    response = parse_http_response(result[:response])
    payload = JSON.parse(response[:body])

    expect(result[:exitstatus]).to eq(0), result[:output]
    expect(response[:status_line]).to eq('HTTP/1.1 200 OK')
    expect(payload).to include(
      'framework' => 'roda',
      'request_method' => 'POST',
      'path_info' => '/framework',
      'trace_id' => 'roda-ru-123',
      'request_body' => 'payload'
    )
  end

  it 'serves a Hanami app from config.ru with the standard Hanami boot path' do
    port = begin
      server = TCPServer.new(VajraE2EHelpers::LISTENER_BIND_HOST, 0)
      server.addr[1]
    ensure
      server&.close
    end

    result = packaged_app_command_request_result_on_port(
      files: hanami_files,
      command: packaged_vajra_command,
      port:,
      env: { 'VAJRA_PORT' => port.to_s },
      request: framework_request('hanami-ru-123')
    )

    response = parse_http_response(result[:response])
    payload = JSON.parse(response[:body])

    expect(result[:exitstatus]).to eq(0), result[:output]
    expect(response[:status_line]).to eq('HTTP/1.1 200 OK')
    expect(payload).to include(
      'framework' => 'hanami',
      'request_method' => 'POST',
      'path_info' => '/framework',
      'trace_id' => 'hanami-ru-123',
      'request_body' => 'payload'
    )
  end

  it 'fails startup with actionable Rails boot diagnostics' do
    failure = startup_failure_with_inline_script(<<~RUBY)
      require "rails"
      require "action_controller/railtie"
      require "vajra/rails"

      class FailingFrameworkCompatibilityRailsApp < ::Rails::Application
        config.root = Dir.pwd
        config.eager_load = false
        config.secret_key_base = "framework-compatibility-secret-key-base"

        def initialize!
          raise "rails boot exploded"
        end
      end

      Vajra::Rails.install!(FailingFrameworkCompatibilityRailsApp)
      Vajra.start
    RUBY

    expect(failure).to match(
      exitstatus: be_positive,
      output: a_string_including(
        'Rails application boot failed: RuntimeError: rails boot exploded'
      )
    )
  end

  it 'fails startup with actionable Rails diagnostics from config/vajra.rb' do
    failure = packaged_app_startup_failure(
      files: {
        'config/vajra.rb' => <<~RUBY,
          Vajra.configure do |config|
            config.rails
          end
        RUBY
        'config/environment.rb' => <<~RUBY
          require "rails"
          require "action_controller/railtie"

          class FailingFrameworkCompatibilityRailsConfigApp < ::Rails::Application
            config.root = Dir.pwd
            config.eager_load = false
            config.secret_key_base = "framework-compatibility-secret-key-base"

            def initialize!
              raise "rails boot exploded"
            end
          end
        RUBY
      }
    )

    expect(failure).to match(
      exitstatus: be_positive,
      output: a_string_including(
        'Rails application boot failed: RuntimeError: rails boot exploded'
      )
    )
  end
end
