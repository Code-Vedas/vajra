# frozen_string_literal: true

# Copyright Codevedas Inc. 2025-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

require 'json'
require_relative 'support'

RSpec.describe 'Vajra Rack environment integration', :e2e, :integration do # rubocop:disable RSpec/DescribeClass
  it 'translates native request and connection state into Rack env fields' do
    result = rack_env_request_result(
      request:
        "POST /projects?filter=active HTTP/1.1\r\n" \
        "Host: example.test\r\n" \
        "X-Trace-Id: abc123\r\n" \
        "X-Trace-Id: def456\r\n" \
        "Content-Type: application/json\r\n" \
        "Content-Length: 0\r\n" \
        "Connection: close\r\n\r\n"
    )

    response = parse_http_response(result[:response])
    env_snapshot = JSON.parse(response[:body])

    expect(result[:exitstatus]).to eq(0)
    expect(response[:status_line]).to eq('HTTP/1.1 200 OK')
    expect(response[:headers]).to include(
      'content-type' => 'application/json',
      'content-length' => response[:body].bytesize.to_s,
      'connection' => 'close'
    )
    expect(env_snapshot).to include(
      'REQUEST_METHOD' => 'POST',
      'SCRIPT_NAME' => '',
      'PATH_INFO' => '/projects',
      'QUERY_STRING' => 'filter=active',
      'SERVER_PROTOCOL' => 'HTTP/1.1',
      'SERVER_NAME' => VajraE2EHelpers::LISTENER_HOST,
      'SERVER_PORT' => result[:port].to_s,
      'REMOTE_ADDR' => VajraE2EHelpers::LISTENER_HOST,
      'rack.url_scheme' => 'http',
      'HTTP_HOST' => 'example.test',
      'HTTP_X_TRACE_ID' => 'abc123,def456',
      'CONTENT_TYPE' => 'application/json',
      'CONTENT_LENGTH' => '0'
    )
    expect(env_snapshot.fetch('REMOTE_PORT')).to match(/\A\d+\z/)
  end

  it 'keeps the default success path when no Rack app is installed' do
    script = <<~RUBY
      require "vajra"
      Vajra.start
    RUBY

    result = rack_app_request_result(
      script:,
      request:
        "GET / HTTP/1.1\r\n" \
        "Host: example.test\r\n" \
        "X_Foo: kept\r\n" \
        "Connection: close\r\n\r\n"
    )

    response = parse_http_response(result[:response])

    expect(result[:exitstatus]).to eq(0)
    expect(response[:status_line]).to eq('HTTP/1.1 200 OK')
    expect(response[:body]).to eq('OK')
  end

  it 'preserves binary rack response bodies' do
    script = <<~RUBY
      require "vajra"

      Vajra::Internal::RackExecution.install!(
        lambda do |_rack_env|
          [200, { "Content-Type" => "application/octet-stream" }, ["a\\0b".b]]
        end
      )

      Vajra.start
    RUBY

    result = rack_app_request_result(
      script:,
      request:
        "GET /binary HTTP/1.1\r\n" \
        "Host: example.test\r\n" \
        "Connection: close\r\n\r\n"
    )

    response = parse_http_response(result[:response])

    expect(result[:exitstatus]).to eq(0)
    expect(response[:status_line]).to eq('HTTP/1.1 200 OK')
    expect(response[:headers]).to include(
      'content-type' => 'application/octet-stream',
      'content-length' => '3',
      'connection' => 'close'
    )
    expect(response[:body].bytes).to eq([97, 0, 98])
  end

  it 'uses a standard reason phrase for redirect Rack responses' do
    script = <<~RUBY
      require "vajra"

      Vajra::Internal::RackExecution.install!(
        lambda do |_rack_env|
          [302, { "Location" => "/moved" }, ["redirecting"]]
        end
      )

      Vajra.start
    RUBY

    result = rack_app_request_result(
      script:,
      request:
        "GET /redirect HTTP/1.1\r\n" \
        "Host: example.test\r\n" \
        "Connection: close\r\n\r\n"
    )

    response = parse_http_response(result[:response])

    expect(result[:exitstatus]).to eq(0)
    expect(response[:status_line]).to eq('HTTP/1.1 302 Found')
    expect(response[:headers]).to include(
      'location' => '/moved',
      'content-length' => '11',
      'connection' => 'close'
    )
    expect(response[:body]).to eq('redirecting')
  end

  it 'rejects Rack responses with out-of-range status codes' do
    script = <<~RUBY
      require "vajra"

      Vajra::Internal::RackExecution.install!(
        lambda do |_rack_env|
          [700, { "Content-Type" => "text/plain" }, ["unexpected"]]
        end
      )

      Vajra.start
    RUBY

    result = rack_app_request_result(
      script:,
      request:
        "GET /invalid-status HTTP/1.1\r\n" \
        "Host: example.test\r\n" \
        "Connection: close\r\n\r\n"
    )

    response = parse_http_response(result[:response])

    expect(result[:exitstatus]).to eq(0)
    expect(response[:status_line]).to eq('HTTP/1.1 500 Internal Server Error')
    expect(response[:headers]).to include('connection' => 'close')
    expect(result[:output]).to include('Rack request execution failed')
    expect(result[:output]).to include('out-of-range HTTP status code')
  end

  it 'preserves binary request header values in the Rack env' do
    script = <<~RUBY
      require "json"
      require "vajra"

      Vajra::Internal::RackExecution.install!(
        lambda do |rack_env|
          value = rack_env.fetch("HTTP_X_OBS_TEXT")
          [200, { "Content-Type" => "application/json" }, [JSON.generate({
            "encoding" => value.encoding.name,
            "bytes" => value.bytes
          })]]
        end
      )

      Vajra.start
    RUBY

    request = [
      "GET /binary-header HTTP/1.1\r\n",
      "Host: example.test\r\n",
      'X-Obs-Text: '.b,
      [0x80, 0xFF].pack('C*'),
      "\r\n",
      "Connection: close\r\n\r\n"
    ].join

    result = rack_app_request_result(script:, request:)
    response = parse_http_response(result[:response])
    snapshot = JSON.parse(response[:body])

    expect(result[:exitstatus]).to eq(0)
    expect(response[:status_line]).to eq('HTTP/1.1 200 OK')
    expect(snapshot).to eq(
      'encoding' => 'ASCII-8BIT',
      'bytes' => [128, 255]
    )
  end

  it 'rejects non-zero content length until rack body transport exists' do
    script = <<~RUBY
      require "vajra"

      Vajra::Internal::RackExecution.install!(
        lambda do |_rack_env|
          [200, { "Content-Type" => "text/plain" }, ["unexpected"]]
        end
      )

      Vajra.start
    RUBY

    result = rack_app_request_result(
      script:,
      request:
        "POST /projects HTTP/1.1\r\n" \
        "Host: example.test\r\n" \
        "Content-Length: 3\r\n" \
        "Connection: close\r\n\r\n" \
        'abc'
    )

    response = parse_http_response(result[:response])

    expect(result[:exitstatus]).to eq(0)
    expect(response[:status_line]).to eq('HTTP/1.1 400 Bad Request')
    expect(response[:headers]).to include('connection' => 'close')
  end
end
