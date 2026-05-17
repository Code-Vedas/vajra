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

  it 'rejects Rack responses with non-integer status codes' do
    script = <<~RUBY
      require "vajra"

      Vajra::Internal::RackExecution.__native_set_callback__(
        proc do |_rack_env|
          ["200", [["Content-Type", "text/plain"]], "unexpected"]
        end
      )

      Vajra.start
    RUBY

    result = rack_app_request_result(
      script:,
      request:
        "GET /invalid-status-type HTTP/1.1\r\n" \
        "Host: example.test\r\n" \
        "Connection: close\r\n\r\n"
    )

    response = parse_http_response(result[:response])

    expect(result[:exitstatus]).to eq(0)
    expect(response[:status_line]).to eq('HTTP/1.1 500 Internal Server Error')
    expect(response[:headers]).to include('connection' => 'close')
    expect(result[:output]).to include('Rack request execution failed')
    expect(result[:output]).to include('non-integer HTTP status code')
  end

  it 'rejects Rack responses with unrepresentable integer status codes' do
    script = <<~RUBY
      require "vajra"

      Vajra::Internal::RackExecution.__native_set_callback__(
        proc do |_rack_env|
          [2**200, [["Content-Type", "text/plain"]], "unexpected"]
        end
      )

      Vajra.start
    RUBY

    result = rack_app_request_result(
      script:,
      request:
        "GET /invalid-large-status HTTP/1.1\r\n" \
        "Host: example.test\r\n" \
        "Connection: close\r\n\r\n"
    )

    response = parse_http_response(result[:response])

    expect(result[:exitstatus]).to eq(0)
    expect(response[:status_line]).to eq('HTTP/1.1 500 Internal Server Error')
    expect(response[:headers]).to include('connection' => 'close')
    expect(result[:output]).to include('Rack request execution failed')
    expect(result[:output]).to include('unrepresentable HTTP status code')
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

  it 'exposes fixed-length request bodies through rack.input' do
    script = <<~RUBY
      require "json"
      require "vajra"

      Vajra::Internal::RackExecution.install!(
        lambda do |rack_env|
          input = rack_env.fetch("rack.input")
          first = input.read(2)
          remainder = input.read
          input.rewind
          [200, { "Content-Type" => "application/json" }, [JSON.generate({
            "encoding" => input.external_encoding.name,
            "first" => first,
            "remainder" => remainder,
            "full" => input.read
          })]]
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
    snapshot = JSON.parse(response[:body])

    expect(result[:exitstatus]).to eq(0)
    expect(response[:status_line]).to eq('HTTP/1.1 200 OK')
    expect(response[:headers]).to include('connection' => 'close')
    expect(snapshot).to eq(
      'encoding' => 'ASCII-8BIT',
      'first' => 'ab',
      'remainder' => 'c',
      'full' => 'abc'
    )
  end

  it 'supports large fragmented fixed-length request bodies' do
    script = <<~RUBY
      require "json"
      require "vajra"

      Vajra::Internal::RackExecution.install!(
        lambda do |rack_env|
          body = rack_env.fetch("rack.input").read
          [200, { "Content-Type" => "application/json" }, [JSON.generate({
            "bytesize" => body.bytesize,
            "prefix" => body.byteslice(0, 4),
            "suffix" => body.byteslice(-4, 4)
          })]]
        end
      )

      Vajra.start
    RUBY

    request_body = ('body' * 3_000).b
    first_chunk = String.new(
      "POST /projects HTTP/1.1\r\n" \
      "Host: example.test\r\n" \
      "Content-Length: #{request_body.bytesize}\r\n" \
      "Connection: close\r\n\r\n",
      encoding: Encoding::BINARY
    )
    first_chunk << request_body.byteslice(0, 3_000)
    result = rack_app_request_chunks_result(
      script:,
      chunks: [
        first_chunk,
        request_body.byteslice(3_000, 4_000),
        request_body.byteslice(7_000..)
      ],
      pause: 0.01
    )

    response = parse_http_response(result[:response])
    snapshot = JSON.parse(response[:body])

    expect(result[:exitstatus]).to eq(0)
    expect(response[:status_line]).to eq('HTTP/1.1 200 OK')
    expect(response[:headers]).to include('connection' => 'close')
    expect(snapshot).to eq(
      'bytesize' => request_body.bytesize,
      'prefix' => 'body',
      'suffix' => 'body'
    )
  end

  it 'decodes chunked request bodies and consumes trailers without surfacing them' do
    script = <<~RUBY
      require "json"
      require "vajra"

      Vajra::Internal::RackExecution.install!(
        lambda do |rack_env|
          input = rack_env.fetch("rack.input")
          [200, { "Content-Type" => "application/json" }, [JSON.generate({
            "body" => input.read,
            "trailer_present" => rack_env.key?("HTTP_X_TRAILER")
          })]]
        end
      )

      Vajra.start
    RUBY

    result = rack_app_request_result(
      script:,
      request:
        "POST /chunked HTTP/1.1\r\n" \
        "Host: example.test\r\n" \
        "Transfer-Encoding: chunked\r\n" \
        "Connection: close\r\n\r\n" \
        "3;foo=bar\r\nabc\r\n" \
        "3\r\n123\r\n" \
        "0\r\n" \
        "X-Trailer: hidden\r\n\r\n"
    )

    response = parse_http_response(result[:response])
    snapshot = JSON.parse(response[:body])

    expect(result[:exitstatus]).to eq(0)
    expect(response[:status_line]).to eq('HTTP/1.1 200 OK')
    expect(response[:headers]).to include('connection' => 'close')
    expect(snapshot).to eq(
      'body' => 'abc123',
      'trailer_present' => false
    )
  end

  it 'decodes fragmented chunked request bodies' do
    script = <<~RUBY
      require "json"
      require "vajra"

      Vajra::Internal::RackExecution.install!(
        lambda do |rack_env|
          input = rack_env.fetch("rack.input")
          first = input.read(4)
          [200, { "Content-Type" => "application/json" }, [JSON.generate({
            "first" => first,
            "rest" => input.read
          })]]
        end
      )

      Vajra.start
    RUBY

    result = rack_app_request_chunks_result(
      script:,
      chunks: [
        "POST /chunked-fragmented HTTP/1.1\r\n" \
        "Host: example.test\r\n" \
        "Transfer-Encoding: chunked\r\n" \
        "Connection: close\r\n\r\n" \
        "4\r\nte",
        "st\r\n5\r\n123",
        "45\r\n0\r\n\r\n"
      ],
      pause: 0.01
    )

    response = parse_http_response(result[:response])
    snapshot = JSON.parse(response[:body])

    expect(result[:exitstatus]).to eq(0)
    expect(response[:status_line]).to eq('HTTP/1.1 200 OK')
    expect(snapshot).to eq(
      'first' => 'test',
      'rest' => '12345'
    )
  end

  it 'rejects malformed chunked request bodies' do
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
        "Transfer-Encoding: chunked\r\n" \
        "Connection: close\r\n\r\n" \
        "Z\r\nabc\r\n0\r\n\r\n"
    )

    response = parse_http_response(result[:response])

    expect(result[:exitstatus]).to eq(0)
    expect(response[:status_line]).to eq('HTTP/1.1 400 Bad Request')
    expect(response[:headers]).to include('connection' => 'close')
  end

  it 'rejects ambiguous content length and transfer encoding framing' do
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
        "Transfer-Encoding: chunked\r\n" \
        "Connection: close\r\n\r\n" \
        "3\r\nabc\r\n0\r\n\r\n"
    )

    response = parse_http_response(result[:response])

    expect(result[:exitstatus]).to eq(0)
    expect(response[:status_line]).to eq('HTTP/1.1 400 Bad Request')
    expect(response[:headers]).to include('connection' => 'close')
  end

  it 'rejects duplicate host headers during rack env translation' do
    result = rack_env_request_result(
      request:
        "GET /projects HTTP/1.1\r\n" \
        "Host: example.test\r\n" \
        "Host: evil.test\r\n" \
        "Connection: close\r\n\r\n"
    )

    response = parse_http_response(result[:response])

    expect(result[:exitstatus]).to eq(0)
    expect(response[:status_line]).to eq('HTTP/1.1 400 Bad Request')
    expect(response[:headers]).to include('connection' => 'close')
  end
end
