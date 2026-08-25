# frozen_string_literal: true

# Copyright Codevedas Inc. 2025-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

require 'json'
require_relative 'support'

RSpec.describe 'Vajra Rack environment integration', :e2e, :integration do
  def read_http1_stream_until(socket, raw_response, marker)
    Timeout.timeout(5) { raw_response << socket.readpartial(4096) until raw_response.include?(marker) }
  end

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
        "X-Foo: kept\r\n" \
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

  {
    'headers.each' => <<~RUBY,
      headers = Class.new do
        def each
          raise "headers exploded"
        end
      end.new
      [200, headers, ["ignored"]]
    RUBY
    'body.each' => <<~RUBY,
      body = Class.new do
        def each
          raise "body exploded"
        end

        def close
          @closed = true
        end
      end.new
      [200, { "Content-Type" => "text/plain" }, body]
    RUBY
    'body.close' => <<~RUBY
      body = Class.new do
        def each
          # A close failure before a payload is committed can still become a
          # normal 500 response.  Once a chunk is committed, the dedicated
          # streaming regression below verifies that the connection is
          # aborted instead.
        end

        def close
          raise "close exploded"
        end
      end.new
      [200, { "Content-Type" => "text/plain" }, body]
    RUBY
  }.each do |failure_site, rack_result|
    it "returns a 500 when native response conversion raises from #{failure_site}" do
      script = <<~RUBY
        require "vajra"

        Vajra::Internal::RackExecution.install!(
          lambda do |_rack_env|
            #{rack_result}
          end
        )

        Vajra.start
      RUBY

      result = rack_app_request_result(
        script:,
        request:
          "GET /raise HTTP/1.1\r\n" \
          "Host: example.test\r\n" \
          "Connection: close\r\n\r\n"
      )

      response = parse_http_response(result[:response])

      expect(result[:exitstatus]).to eq(0), result[:output]
      expect(response[:status_line]).to eq('HTTP/1.1 500 Internal Server Error')
    end
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

  it 'does not retain arbitrary Rack env header keys after requests finish' do
    script = <<~RUBY
      require "objspace"
      require "vajra"

      Vajra::Internal::RackExecution.install!(
        lambda do |rack_env|
          if rack_env.fetch("PATH_INFO") == "/string-count"
            GC.start(full_mark: true, immediate_sweep: true)
            [200, { "Content-Type" => "text/plain" }, [ObjectSpace.each_object(String).count.to_s]]
          else
            [200, { "Content-Type" => "text/plain" }, [rack_env.fetch("PATH_INFO")]]
          end
        end
      )

      Vajra.start
    RUBY

    request_for = lambda do |path, header_name|
      "GET #{path} HTTP/1.1\r\n" \
        "Host: example.test\r\n" \
        "#{header_name}: request-local\r\n" \
        "Connection: close\r\n\r\n"
    end

    managed_popen2e(vajra_env, *inline_ruby_command(script), chdir: VajraE2EHelpers::PACKAGE_ROOT) do |_stdin, output, wait_thread|
      startup_output = []
      selected_port = wait_for_banner(output, captured_lines: startup_output)
      request = lambda do |raw_request, label|
        socket = TCPSocket.new(VajraE2EHelpers::LISTENER_HOST, selected_port)
        socket.write(raw_request)
        read_http_response(socket, wait_thread:, output:, request_label: label).first
      ensure
        socket&.close unless socket&.closed?
      end

      baseline = request.call(request_for.call('/string-count', 'X-Audit-Baseline'), 'header_cache:baseline')
      96.times do |index|
        response = request.call(request_for.call('/request', "X-Audit-Unique-#{index}"), "header_cache:#{index}")
        expect(response[:body]).to eq('/request')
      end
      after = request.call(request_for.call('/string-count', 'X-Audit-After'), 'header_cache:after')

      expect(after[:body].to_i).to be <= baseline[:body].to_i + 24
      expect(stop_process(wait_thread).exitstatus).to eq(0)
    ensure
      cleanup_process(wait_thread, output)
    end
  end

  it 'keeps concurrently supplied arbitrary headers isolated in Rack envs' do
    script = <<~RUBY
      require "vajra"

      Vajra::Internal::RackExecution.install!(
        lambda do |rack_env|
          header_key = rack_env.keys.find { |key| key == "HTTP_X_AUDIT_TOKEN" }
          [200, { "Content-Type" => "text/plain" }, ["\#{header_key.frozen?}:\#{rack_env.fetch(header_key)}"]]
        end
      )

      Vajra.start
    RUBY
    requests = Array.new(32) do |index|
      "GET /headers HTTP/1.1\r\n" \
        "Host: example.test\r\n" \
        "X-Audit-Token: token-#{index}\r\n" \
        "Connection: close\r\n\r\n"
    end

    result = concurrent_rack_app_request_results(script:, requests:)

    expect(result[:exitstatus]).to eq(0)
    expect(result[:responses].map { |response| parse_http_response(response)[:body] }).to eq(
      Array.new(32) { |index| "true:token-#{index}" }
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

  it 'rejects request bodies over the configured native limit and cancels Rack input readers' do
    Dir.mktmpdir('vajra-body-limit') do |root|
      marker_path = File.join(root, 'rack-called')
      script = <<~RUBY
        require "vajra"

        Vajra::Internal::RackExecution.install!(
          lambda do |rack_env|
            begin
              rack_env.fetch("rack.input").read
              File.write(ENV.fetch("RACK_CALLED_MARKER"), "read")
            rescue => e
              File.write(ENV.fetch("RACK_CALLED_MARKER"), "\#{e.class}:\#{e.message}")
            end
            [200, { "Content-Type" => "text/plain" }, ["unexpected"]]
          end
        )

        Vajra.start(max_request_body_bytes: 3)
      RUBY

      result = rack_app_request_result(
        script:,
        env: { 'RACK_CALLED_MARKER' => marker_path },
        request:
          "POST /too-large HTTP/1.1\r\n" \
          "Host: example.test\r\n" \
          "Content-Length: 4\r\n" \
          "Connection: close\r\n\r\n" \
          'body'
      )

      response = parse_http_response(result[:response])

      expect(result[:exitstatus]).to eq(0)
      expect(response[:status_line]).to eq('HTTP/1.1 400 Bad Request')
      expect(File.read(marker_path)).to eq('IOError:request body stream closed before completion')
    end
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

  it 'preserves early Rack responses while draining unread large request bodies' do
    script = <<~RUBY
      require "vajra"

      Vajra::Internal::RackExecution.install!(
        lambda do |_rack_env|
          [202, { "Content-Type" => "text/plain" }, ["accepted"]]
        end
      )

      Vajra.start
    RUBY

    request_body = ('x' * (2 * 1024 * 1024)).b
    result = rack_app_request_chunks_result(
      script:,
      chunks: [
        "POST /early HTTP/1.1\r\n" \
        "Host: example.test\r\n" \
        "Content-Length: #{request_body.bytesize}\r\n" \
        "Connection: close\r\n\r\n",
        request_body
      ],
      pause: 0.01
    )

    response = parse_http_response(result[:response])

    expect(result[:exitstatus]).to eq(0), result[:output]
    expect(response[:status_line]).to eq('HTTP/1.1 202 Accepted')
    expect(response[:body]).to eq('accepted')
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

  it 'streams an unknown Rack enumerable over HTTP/1 before the producer finishes' do
    Dir.mktmpdir('vajra-http1-stream') do |root|
      ready_path = File.join(root, 'ready')
      release_path = File.join(root, 'release')
      closed_path = File.join(root, 'closed')
      script = <<~RUBY
        require "vajra"

        class GatedRackBody
          def each
            yield "first".b
            File.binwrite(ENV.fetch("STREAM_READY_PATH"), "ready")
            sleep 0.005 until File.exist?(ENV.fetch("STREAM_RELEASE_PATH"))
            yield "second".b
          end

          def close
            File.open(ENV.fetch("STREAM_CLOSED_PATH"), "a") { |file| file.puts("closed") }
          end
        end

        Vajra::Internal::RackExecution.install!(
          lambda do |_rack_env|
            [200, { "Content-Type" => "text/plain" }, GatedRackBody.new]
          end
        )

        Vajra.start(workers: 1, threads: [1, 1])
      RUBY

      managed_popen2e(
        vajra_env(port: disposable_listener_port).merge(
          'STREAM_READY_PATH' => ready_path,
          'STREAM_RELEASE_PATH' => release_path,
          'STREAM_CLOSED_PATH' => closed_path
        ),
        *inline_ruby_command(script),
        chdir: VajraE2EHelpers::PACKAGE_ROOT
      ) do |_stdin, output, wait_thread|
        selected_port = wait_for_banner(output)
        socket = TCPSocket.new(VajraE2EHelpers::LISTENER_HOST, selected_port)
        raw_response = String.new(encoding: Encoding::BINARY)
        begin
          socket.write("GET /gated HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
          read_http1_stream_until(socket, raw_response, "\r\n\r\n")
          headers, = raw_response.split("\r\n\r\n", 2)
          expect(headers).to start_with('HTTP/1.1 200 OK')
          expect(headers).to include("Transfer-Encoding: chunked\r\n")
          expect(headers).not_to include('Content-Length:')

          read_http1_stream_until(socket, raw_response, "5\r\nfirst\r\n")
          Timeout.timeout(5) { sleep 0.005 until File.exist?(ready_path) }
          expect(raw_response).not_to include("6\r\nsecond\r\n")
          expect(File).not_to exist(release_path)

          File.binwrite(release_path, 'release')
          read_http1_stream_until(socket, raw_response, "0\r\n\r\n")
          expect(raw_response).to include("5\r\nfirst\r\n6\r\nsecond\r\n0\r\n\r\n")
          Timeout.timeout(5) { sleep 0.005 until File.exist?(closed_path) }
          expect(File.readlines(closed_path)).to eq(["closed\n"])
        ensure
          socket.close unless socket.closed?
        end

        status = stop_process(wait_thread)
        expect(status.exitstatus).to eq(0), output.read
      ensure
        cleanup_process(wait_thread, output)
      end
    end
  end

  it 'closes a Rack body once and aborts an HTTP/1 stream when it raises after commit' do
    Dir.mktmpdir('vajra-http1-stream-error') do |root|
      closed_path = File.join(root, 'closed')
      script = <<~RUBY
        require "vajra"

        class FailingRackBody
          def each
            yield "first".b
            raise "body exploded after first chunk"
          end

          def close
            File.open(ENV.fetch("STREAM_CLOSED_PATH"), "a") { |file| file.puts("closed") }
          end
        end

        Vajra::Internal::RackExecution.install!(
          lambda do |_rack_env|
            [200, { "Content-Type" => "text/plain" }, FailingRackBody.new]
          end
        )

        Vajra.start(workers: 1, threads: [1, 1])
      RUBY

      managed_popen2e(
        vajra_env(port: disposable_listener_port).merge('STREAM_CLOSED_PATH' => closed_path),
        *inline_ruby_command(script),
        chdir: VajraE2EHelpers::PACKAGE_ROOT
      ) do |_stdin, output, wait_thread|
        selected_port = wait_for_banner(output)
        socket = TCPSocket.new(VajraE2EHelpers::LISTENER_HOST, selected_port)
        raw_response = String.new(encoding: Encoding::BINARY)
        begin
          socket.write("GET /failure HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
          Timeout.timeout(5) do
            loop { raw_response << socket.readpartial(4096) }
          rescue EOFError, Errno::ECONNRESET
            nil
          end
          expect(raw_response).to start_with('HTTP/1.1 200 OK')
          expect(raw_response).to include("5\r\nfirst\r\n")
          expect(raw_response).not_to include("0\r\n\r\n")
          Timeout.timeout(5) { sleep 0.005 until File.exist?(closed_path) }
          expect(File.readlines(closed_path)).to eq(["closed\n"])
        ensure
          socket.close unless socket.closed?
        end

        status = stop_process(wait_thread)
        expect(status.exitstatus).to eq(0), output.read
      ensure
        cleanup_process(wait_thread, output)
      end
    end
  end

  it 'closes a Rack body once and aborts an HTTP/1 stream for a non-string chunk after commit' do
    Dir.mktmpdir('vajra-http1-stream-non-string') do |root|
      closed_path = File.join(root, 'closed')
      script = <<~RUBY
        require "vajra"

        class NonStringRackBody
          def each
            yield "first".b
            yield 42
          end

          def close
            File.open(ENV.fetch("STREAM_CLOSED_PATH"), "a") { |file| file.puts("closed") }
          end
        end

        Vajra::Internal::RackExecution.install!(
          lambda do |_rack_env|
            [200, { "Content-Type" => "text/plain" }, NonStringRackBody.new]
          end
        )

        Vajra.start(workers: 1, threads: [1, 1])
      RUBY

      managed_popen2e(
        vajra_env(port: disposable_listener_port).merge('STREAM_CLOSED_PATH' => closed_path),
        *inline_ruby_command(script),
        chdir: VajraE2EHelpers::PACKAGE_ROOT
      ) do |_stdin, output, wait_thread|
        selected_port = wait_for_banner(output)
        socket = TCPSocket.new(VajraE2EHelpers::LISTENER_HOST, selected_port)
        raw_response = String.new(encoding: Encoding::BINARY)
        begin
          socket.write("GET /non-string HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
          Timeout.timeout(5) do
            loop { raw_response << socket.readpartial(4096) }
          rescue EOFError, Errno::ECONNRESET
            nil
          end
          expect(raw_response).to start_with('HTTP/1.1 200 OK')
          expect(raw_response).to include("5\r\nfirst\r\n")
          expect(raw_response).not_to include("2\r\n42\r\n")
          expect(raw_response).not_to include("0\r\n\r\n")
          Timeout.timeout(5) { sleep 0.005 until File.exist?(closed_path) }
          expect(File.readlines(closed_path)).to eq(["closed\n"])
        ensure
          socket.close unless socket.closed?
        end

        status = stop_process(wait_thread)
        expect(status.exitstatus).to eq(0), output.read
      ensure
        cleanup_process(wait_thread, output)
      end
    end
  end

  it 'cancels a published oversized Rack body when HTTP/1 response validation falls back' do
    Dir.mktmpdir('vajra-http1-stream-invalid-response') do |root|
      started_path = File.join(root, 'started')
      closed_path = File.join(root, 'closed')
      script = <<~RUBY
        require "vajra"

        class OversizedInvalidRackBody
          def each
            File.binwrite(ENV.fetch("STREAM_STARTED_PATH"), "started")
            yield "x".b * 65_537
          end

          def close
            File.open(ENV.fetch("STREAM_CLOSED_PATH"), "a") { |file| file.puts("closed") }
          end
        end

        Vajra::Internal::RackExecution.install!(
          lambda do |rack_env|
            if rack_env.fetch("PATH_INFO") == "/invalid"
              [200, { "Bad Header" => "unsafe" }, OversizedInvalidRackBody.new]
            else
              [200, { "Content-Type" => "text/plain" }, ["healthy"]]
            end
          end
        )

        Vajra.start(workers: 1, threads: [1, 1])
      RUBY

      managed_popen2e(
        vajra_env(port: disposable_listener_port).merge(
          'STREAM_STARTED_PATH' => started_path,
          'STREAM_CLOSED_PATH' => closed_path
        ),
        *inline_ruby_command(script),
        chdir: VajraE2EHelpers::PACKAGE_ROOT
      ) do |_stdin, output, wait_thread|
        selected_port = wait_for_banner(output)
        socket = TCPSocket.new(VajraE2EHelpers::LISTENER_HOST, selected_port)
        begin
          socket.write("GET /invalid HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
          response, = read_http_response(socket, wait_thread:, output:, request_label: 'invalid streamed response')
          expect(response[:status_line]).to eq('HTTP/1.1 500 Internal Server Error')
          Timeout.timeout(5) { sleep 0.005 until File.exist?(started_path) }
          Timeout.timeout(5) { sleep 0.005 until File.exist?(closed_path) }
          expect(File.readlines(closed_path)).to eq(["closed\n"])
        ensure
          socket.close unless socket.closed?
        end

        healthy_socket = TCPSocket.new(VajraE2EHelpers::LISTENER_HOST, selected_port)
        begin
          healthy_socket.write("GET /healthy HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
          healthy_response, = read_http_response(healthy_socket, wait_thread:, output:, request_label: 'post-cancel health check')
          expect(healthy_response[:status_line]).to eq('HTTP/1.1 200 OK')
          expect(healthy_response[:body]).to eq('healthy')
        ensure
          healthy_socket.close unless healthy_socket.closed?
        end

        status = stop_process(wait_thread)
        expect(status.exitstatus).to eq(0), output.read
      ensure
        cleanup_process(wait_thread, output)
      end
    end
  end

  it 'keeps a matching finite Rack Content-Length and frames a mismatch as chunked' do
    script = <<~RUBY
      require "vajra"

      Vajra::Internal::RackExecution.install!(
        lambda do |rack_env|
          if rack_env.fetch("PATH_INFO") == "/matching"
            [200, { "Content-Type" => "text/plain", "Content-Length" => "5" }, ["hello".b]]
          else
            [200, { "Content-Type" => "text/plain", "Content-Length" => "99" }, ["hello".b]]
          end
        end
      )

      Vajra.start(workers: 1, threads: [1, 1])
    RUBY

    matching = rack_app_request_result(
      script:,
      request: "GET /matching HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"
    )
    matching_response = parse_http_response(matching[:response])
    expect(matching[:exitstatus]).to eq(0)
    expect(matching_response[:headers]).to include('content-length' => '5')
    expect(matching_response[:headers]).not_to have_key('transfer-encoding')
    expect(matching_response[:body]).to eq('hello')

    mismatched = rack_app_request_result(
      script:,
      request: "GET /mismatched HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"
    )
    mismatched_response = parse_http_response(mismatched[:response])
    expect(mismatched[:exitstatus]).to eq(0)
    expect(mismatched_response[:headers]).to include('transfer-encoding' => 'chunked')
    expect(mismatched_response[:headers]).not_to have_key('content-length')
    expect(mismatched_response[:body]).to eq('hello')
  end

  it 'uses chunked framing when an exact Rack Array has a singleton each' do
    Dir.mktmpdir('vajra-http1-stream-untrusted-length') do |root|
      closed_path = File.join(root, 'closed')
      script = <<~RUBY
        require "vajra"

        body = []
        def body.each
          yield "live".b
        end
        def body.close
          File.open(ENV.fetch("STREAM_CLOSED_PATH"), "a") { |file| file.puts("closed") }
        end

        Vajra::Internal::RackExecution.install!(
          lambda do |_rack_env|
            [200, { "Content-Type" => "text/plain", "Content-Length" => "0" }, body]
          end
        )

        Vajra.start(workers: 1, threads: [1, 1])
      RUBY

      managed_popen2e(
        vajra_env(port: disposable_listener_port).merge('STREAM_CLOSED_PATH' => closed_path),
        *inline_ruby_command(script),
        chdir: VajraE2EHelpers::PACKAGE_ROOT
      ) do |_stdin, output, wait_thread|
        selected_port = wait_for_banner(output)
        socket = TCPSocket.new(VajraE2EHelpers::LISTENER_HOST, selected_port)
        begin
          socket.write("GET /divergent HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
          response, = read_http_response(socket, wait_thread:, output:, request_label: 'untrusted live Rack body')
          expect(response[:status_line]).to eq('HTTP/1.1 200 OK')
          expect(response[:headers]).to include('transfer-encoding' => 'chunked')
          expect(response[:headers]).not_to have_key('content-length')
          expect(response[:body]).to eq('live')
          Timeout.timeout(5) { sleep 0.005 until File.exist?(closed_path) }
          expect(File.readlines(closed_path)).to eq(["closed\n"])
        ensure
          socket.close unless socket.closed?
        end

        status = stop_process(wait_thread)
        expect(status.exitstatus).to eq(0), output.read
      ensure
        cleanup_process(wait_thread, output)
      end
    end
  end

  it 'uses chunked framing when Array each is C-rebound' do
    Dir.mktmpdir('vajra-http1-stream-c-rebound-length') do |_root|
      script = <<~RUBY
        require "vajra"

        Array.define_method(:each, Array.instance_method(:reverse_each))
        body = ["a".b, "bb".b]

        Vajra::Internal::RackExecution.install!(
          lambda do |_rack_env|
            [200, { "Content-Type" => "text/plain", "Content-Length" => "3" }, body]
          end
        )

        Vajra.start(workers: 1, threads: [1, 1])
      RUBY

      managed_popen2e(
        vajra_env(port: disposable_listener_port),
        *inline_ruby_command(script),
        chdir: VajraE2EHelpers::PACKAGE_ROOT
      ) do |_stdin, output, wait_thread|
        selected_port = wait_for_banner(output)
        socket = TCPSocket.new(VajraE2EHelpers::LISTENER_HOST, selected_port)
        begin
          socket.write("GET /c-rebound HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
          response, = read_http_response(socket, wait_thread:, output:, request_label: 'C-rebound Rack Array body')
          expect(response[:status_line]).to eq('HTTP/1.1 200 OK')
          expect(response[:headers]).to include('transfer-encoding' => 'chunked')
          expect(response[:headers]).not_to have_key('content-length')
          expect(response[:body]).to eq('bba')
        ensure
          socket.close unless socket.closed?
        end

        status = stop_process(wait_thread)
        expect(status.exitstatus).to eq(0), output.read
      ensure
        cleanup_process(wait_thread, output)
      end
    end
  end
end
