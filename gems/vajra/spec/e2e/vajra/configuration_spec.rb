# frozen_string_literal: true

# Copyright Codevedas Inc. 2025-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

require_relative 'support'
require 'tmpdir'
require 'json'
require 'open3'
require 'openssl'

RSpec.describe 'Vajra configuration', :e2e, :integration do
  def post_request(body)
    "POST / HTTP/1.1\r\n" \
      "Host: localhost\r\n" \
      "Content-Type: text/plain\r\n" \
      "Content-Length: #{body.bytesize}\r\n" \
      "Connection: close\r\n\r\n" \
      "#{body}"
  end

  def fairness_order_script
    <<~RUBY
      require "vajra"

      order_mutex = Mutex.new

      Vajra::Internal::RackExecution.install!(
        lambda do |rack_env|
          body = rack_env.fetch("rack.input").read
          order_mutex.synchronize do
            File.open(ENV.fetch("ORDER_PATH"), "a") { |file| file.puts(body) }
          end

          case body
          when "slow-a"
            sleep 1.0
          when "slow-b"
            sleep 0.2
          end

          [200, { "Content-Type" => "text/plain" }, [body]]
        end
      )

      Vajra.start(
        workers: 2,
        threads: [1, 1],
        socket_queue_capacity: 10,
        log_level: "debug"
      )
    RUBY
  end

  def queue_capacity_script
    <<~RUBY
      require "vajra"

      Vajra::Internal::RackExecution.install!(
        lambda do |_rack_env|
          sleep 0.75
          [200, { "Content-Type" => "text/plain" }, ["OK"]]
        end
      )

      Vajra.start(
        workers: 1,
        threads: [1, 1],
        socket_queue_capacity: 1,
        log_level: "debug"
      )
    RUBY
  end

  def worker_thread_pool_script
    <<~RUBY
      require "json"
      require "vajra"

      state_mutex = Mutex.new
      state_condition = ConditionVariable.new
      active = 0
      max_active = 0
      started = []
      release_holds = false
      release_thread_started = false

      Vajra::Internal::RackExecution.install!(
        lambda do |rack_env|
          body = rack_env.fetch("rack.input").read
          snapshot = nil

          state_mutex.synchronize do
            active += 1
            max_active = [max_active, active].max
            started << body

            if body.start_with?("hold") && started.count { |value| value.start_with?("hold") } == 2 && !release_thread_started
              release_thread_started = true
              Thread.new do
                sleep 0.35
                state_mutex.synchronize do
                  release_holds = true
                  state_condition.broadcast
                end
              end
            end

            while body.start_with?("hold") && !release_holds
              state_condition.wait(state_mutex)
            end

            snapshot = { max_active:, started: started.dup }
            active -= 1
          end

          [200, { "Content-Type" => "application/json" }, [JSON.generate(body:, max_active: snapshot.fetch(:max_active), started: snapshot.fetch(:started))]]
        end
      )

      Vajra.start(
        workers: 1,
        threads: [2, 2],
        socket_queue_capacity: 10,
        log_level: "debug"
      )
    RUBY
  end

  def queue_capacity_request_head(body)
    "POST / HTTP/1.1\r\n" \
      "Host: localhost\r\n" \
      "Content-Type: text/plain\r\n" \
      "Content-Length: #{body.bytesize}\r\n" \
      "Connection: close\r\n\r\n"
  end

  def queue_capacity_runtime_output(output)
    runtime_output = +''

    Timeout.timeout(2) do
      loop do
        runtime_output << read_available_output(output)
        break if runtime_output.include?('event=request_admitted')

        sleep 0.01
      end
    end

    runtime_output
  end

  def wait_for_runtime_output(output, runtime_output, pattern, count: 1, timeout: 2)
    Timeout.timeout(timeout) do
      loop do
        runtime_output << read_available_output(output)
        break if runtime_output.scan(pattern).size >= count

        sleep 0.01
      end
    end
  end

  def write_self_signed_certificate(root)
    key = OpenSSL::PKey::RSA.new(2048)
    cert = self_signed_certificate(key)

    cert_path = File.join(root, 'server.crt')
    key_path = File.join(root, 'server.key')
    File.write(cert_path, cert.to_pem)
    File.write(key_path, key.to_pem)
    [cert_path, key_path]
  end

  def self_signed_certificate(key)
    cert = OpenSSL::X509::Certificate.new
    cert.version = 2
    cert.serial = 1
    cert.subject = OpenSSL::X509::Name.parse('/CN=localhost')
    cert.issuer = cert.subject
    cert.public_key = key.public_key
    cert.not_before = Time.now.utc - 60
    cert.not_after = Time.now.utc + 3600
    add_self_signed_certificate_extensions(cert)
    cert.sign(key, OpenSSL::Digest.new('SHA256'))
    cert
  end

  def add_self_signed_certificate_extensions(cert)
    extension_factory = OpenSSL::X509::ExtensionFactory.new
    extension_factory.subject_certificate = cert
    extension_factory.issuer_certificate = cert
    cert.add_extension(extension_factory.create_extension('subjectAltName', 'DNS:localhost,IP:127.0.0.1'))
    cert.add_extension(extension_factory.create_extension('basicConstraints', 'CA:FALSE', true))
  end

  def connected_tls_socket(port, alpn_protocols: nil)
    tcp_socket = TCPSocket.new(VajraE2EHelpers::LISTENER_HOST, port)
    context = OpenSSL::SSL::SSLContext.new
    context.verify_mode = OpenSSL::SSL::VERIFY_NONE
    context.alpn_protocols = alpn_protocols if alpn_protocols
    ssl_socket = OpenSSL::SSL::SSLSocket.new(tcp_socket, context)
    ssl_socket.hostname = 'localhost'
    ssl_socket.connect
    ssl_socket
  end

  def with_tls_vajra(script:, cert_path:, key_path:)
    managed_popen2e(
      vajra_env(port: disposable_listener_port).merge('TLS_CERTIFICATE' => cert_path, 'TLS_PRIVATE_KEY' => key_path),
      *inline_ruby_command(script),
      chdir: VajraE2EHelpers::PACKAGE_ROOT
    ) do |_stdin, output, wait_thread|
      startup_output = []
      selected_port = wait_for_banner(output, captured_lines: startup_output)
      yield(selected_port, startup_output, output, wait_thread)
    ensure
      cleanup_process(wait_thread, output)
    end
  end

  def tls_request_response(script:, cert_path:, key_path:)
    with_tls_vajra(script:, cert_path:, key_path:) do |selected_port, startup_output, output, wait_thread|
      ssl_socket = connected_tls_socket(selected_port)
      ssl_socket.write("GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
      response = ssl_socket.read
      ssl_socket.close
      status = stop_process(wait_thread)
      { exitstatus: status.exitstatus, response:, output: "#{startup_output.join}#{output.read}", port: selected_port }
    end
  end

  def h2_integer(value, prefix_bits, prefix_value)
    prefix_max = (1 << prefix_bits) - 1
    return [prefix_value | value].pack('C') if value < prefix_max

    bytes = [prefix_value | prefix_max]
    value -= prefix_max
    while value >= 128
      bytes << ((value % 128) + 128)
      value /= 128
    end
    bytes << value
    bytes.pack('C*')
  end

  def h2_string(value)
    h2_integer(value.bytesize, 7, 0) + value
  end

  def h2_frame(type, flags, stream_id, payload)
    [payload.bytesize].pack('N')[1, 3] + [type, flags, stream_id & 0x7fff_ffff].pack('CCN') + payload
  end

  def h2_read_frame(socket)
    header = socket.read(9)
    raise 'HTTP/2 frame header missing' unless header&.bytesize == 9

    a, b, c = header.byteslice(0, 3).bytes
    length = (a << 16) | (b << 8) | c
    type, flags, stream_id = header.byteslice(3, 6).unpack('CCN')
    payload = length.zero? ? '' : socket.read(length)
    { length:, type:, flags:, stream_id: stream_id & 0x7fff_ffff, payload: }
  end

  def h2_request_header_block
    header_block = +"\x82".b # :method GET
    header_block << "\x84".b # :path /
    header_block << "\x87".b # :scheme https
    header_block << "\x41".b << h2_string('localhost') # :authority literal
    header_block
  end

  def h2_start(socket)
    socket.write(
      "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n" \
      "#{h2_frame(4, 0, 0, '')}"
    )

    Timeout.timeout(5) do
      loop do
        frame = h2_read_frame(socket)
        if frame[:type] == 4 && frame[:flags].nobits?(0x1)
          socket.write(h2_frame(4, 0x1, 0, ''))
          return
        end
      end
    end
  end

  def h2_read_until(socket, type:)
    Timeout.timeout(5) do
      loop do
        frame = h2_read_frame(socket)
        return frame if frame[:type] == type
      end
    end
  end

  def h2_goaway_error_code(frame)
    frame.fetch(:payload).byteslice(4, 4).unpack1('N')
  end

  def h2_rst_stream_error_code(frame)
    frame.fetch(:payload).unpack1('N')
  end

  def h2_exchange(socket)
    socket.write(
      "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n" \
      "#{h2_frame(4, 0, 0, '')}" \
      "#{h2_frame(1, 0x5, 1, h2_request_header_block)}"
    )

    frames = []
    Timeout.timeout(5) do
      loop do
        frame = h2_read_frame(socket)
        frames << frame
        socket.write(h2_frame(4, 0x1, 0, '')) if frame[:type] == 4 && frame[:flags].nobits?(0x1)
        break if frame[:type] == 0 && frame[:flags].anybits?(0x1)
      end
    end
    frames
  end

  def h2_request_response(script:, cert_path:, key_path:)
    with_tls_vajra(script:, cert_path:, key_path:) do |selected_port, startup_output, output, wait_thread|
      ssl_socket = connected_tls_socket(selected_port, alpn_protocols: ['h2'])
      raise "unexpected ALPN protocol: #{ssl_socket.alpn_protocol.inspect}" unless ssl_socket.alpn_protocol == 'h2'

      frames = h2_exchange(ssl_socket)
      ssl_socket.close
      status = stop_process(wait_thread)
      { exitstatus: status.exitstatus, frames:, output: "#{startup_output.join}#{output.read}", port: selected_port }
    end
  end

  def h2_protocol_assertions(script:, cert_path:, key_path:)
    with_tls_vajra(script:, cert_path:, key_path:) do |selected_port, startup_output, output, wait_thread|
      yield(selected_port)
      status = stop_process(wait_thread)
      { exitstatus: status.exitstatus, output: "#{startup_output.join}#{output.read}", port: selected_port }
    end
  end

  def curl_http2_response(script:, cert_path:, key_path:)
    with_tls_vajra(script:, cert_path:, key_path:) do |selected_port, startup_output, output, wait_thread|
      stdout, stderr, curl_status = Open3.capture3(
        'curl',
        '--http2',
        '--insecure',
        '--silent',
        '--show-error',
        '--max-time',
        '5',
        '--write-out',
        ["\n", '%', '{http_version}', ' ', '%', '{http_code}'].join,
        "https://localhost:#{selected_port}/"
      )
      status = stop_process(wait_thread)
      {
        exitstatus: status.exitstatus,
        curl_exitstatus: curl_status.exitstatus,
        stdout:,
        stderr:,
        output: "#{startup_output.join}#{output.read}",
        port: selected_port
      }
    end
  end

  def queue_capacity_responses(selected_port, wait_thread, output)
    first_body = 'one'
    sockets = open_request_sockets(selected_port, 3)

    begin
      sockets[0].write(queue_capacity_request_head(first_body))
      runtime_output = queue_capacity_runtime_output(output)
      sockets[1].write(post_request('two'))
      wait_for_runtime_output(output, runtime_output, 'event=queue_capacity_reached')
      sockets[2].write(post_request('three'))
      wait_for_runtime_output(output, runtime_output, 'event=queue_capacity_reached', count: 2)
      sockets[0].write(first_body)
      responses = sockets.each_with_index.map do |socket, index|
        parse_http_response(
          read_raw_http_response(
            socket,
            wait_thread:,
            output:,
            request_label: "queue_capacity_result:#{index}"
          )
        )
      end

      [responses, runtime_output]
    ensure
      sockets.each { |socket| socket.close unless socket.closed? }
    end
  end

  def queue_capacity_result(script:)
    managed_popen2e(
      vajra_env(port: disposable_listener_port),
      *inline_ruby_command(script),
      chdir: VajraE2EHelpers::PACKAGE_ROOT
    ) do |_stdin, output, wait_thread|
      startup_output = []
      selected_port = wait_for_banner(output, captured_lines: startup_output)
      responses, runtime_output = queue_capacity_responses(selected_port, wait_thread, output)
      status = begin
        stop_process(wait_thread)
      rescue Timeout::Error
        signal_process_group(wait_thread, 'KILL')
        wait_thread.value
      end

      {
        exitstatus: status.exitstatus,
        responses:,
        output: "#{startup_output.join}#{runtime_output}#{output.read}"
      }
    ensure
      cleanup_process(wait_thread, output)
    end
  end

  def max_keepalive_requests_result(env:)
    managed_popen2e(vajra_env(port: disposable_listener_port).merge(env), *vajra_command, chdir: VajraE2EHelpers::PACKAGE_ROOT) do |_stdin, output, wait_thread|
      startup_output = []
      selected_port = wait_for_banner(output, captured_lines: startup_output)
      socket = TCPSocket.new(VajraE2EHelpers::LISTENER_HOST, selected_port)
      buffered_bytes = String.new(encoding: Encoding::BINARY)

      socket.write("GET /first HTTP/1.1\r\nHost: localhost\r\n\r\n")
      first_response, buffered_bytes = read_http_response(
        socket,
        buffered_bytes:,
        wait_thread:,
        output:,
        request_label: 'max_keepalive_requests_result:first'
      )
      socket.write("GET /second HTTP/1.1\r\nHost: localhost\r\n\r\n")
      second_response, buffered_bytes = read_http_response(
        socket,
        buffered_bytes:,
        wait_thread:,
        output:,
        request_label: 'max_keepalive_requests_result:second'
      )
      connection_closed = Timeout.timeout(2) { socket.read == '' }
      socket.close
      status = stop_process(wait_thread)

      {
        exitstatus: status.exitstatus,
        first_response:,
        second_response:,
        connection_closed:,
        trailing_bytes: buffered_bytes,
        output: "#{startup_output.join}#{output.read}"
      }
    ensure
      socket&.close unless socket&.closed?
      cleanup_process(wait_thread, output)
    end
  end

  def control_plane_response_result(script:, path:)
    managed_popen2e(
      vajra_env(port: disposable_listener_port),
      *inline_ruby_command(script),
      chdir: VajraE2EHelpers::PACKAGE_ROOT
    ) do |_stdin, output, wait_thread|
      startup_output = []
      selected_port = wait_for_banner(output, captured_lines: startup_output)

      socket = TCPSocket.new(VajraE2EHelpers::LISTENER_HOST, selected_port)
      begin
        socket.write("GET #{path} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
        response = parse_http_response(
          read_raw_http_response(
            socket,
            wait_thread:,
            output:,
            request_label: "control_plane_response_result:#{path}"
          )
        )
      ensure
        socket.close unless socket.closed?
      end

      status = stop_process(wait_thread)
      { exitstatus: status.exitstatus, response:, output: "#{startup_output.join}#{output.read}" }
    ensure
      cleanup_process(wait_thread, output)
    end
  end

  def json_access_log_result
    Dir.mktmpdir('vajra-access-log') do |root|
      access_log_path = File.join(root, 'access.log')
      script = <<~RUBY
        require "vajra"

        Vajra::Internal::RackExecution.install!(
          lambda do |_rack_env|
            [200, { "Content-Type" => "text/plain" }, ["OK"]]
          end
        )

        Vajra.start(
          workers: 1,
          threads: [1, 1],
          access_log: ENV.fetch("ACCESS_LOG_PATH"),
          access_log_format: "json",
          structured_logs: true
        )
      RUBY

      managed_popen2e(
        vajra_env(port: disposable_listener_port).merge('ACCESS_LOG_PATH' => access_log_path),
        *inline_ruby_command(script),
        chdir: VajraE2EHelpers::PACKAGE_ROOT
      ) do |_stdin, output, wait_thread|
        startup_output = []
        selected_port = wait_for_banner(output, captured_lines: startup_output)

        response = otel_observability_request(
          selected_port,
          wait_thread,
          output,
          "GET /observability HTTP/1.1\r\n" \
          "Host: example.test\r\n" \
          "User-Agent: vajra-e2e\r\n" \
          "Referer: https://example.test/source\r\n" \
          "X-Request-Id: request-123\r\n" \
          "Traceparent: 00-11111111111111111111111111111111-2222222222222222-01\r\n" \
          "Connection: close\r\n\r\n",
          'json_access_log_result'
        )
        invalid_response = otel_observability_request(
          selected_port,
          wait_thread,
          output,
          "GET /invalid-traceparent HTTP/1.1\r\n" \
          "Host: example.test\r\n" \
          "Traceparent: 00-not-a-trace-id-2222222222222222-01\r\n" \
          "Connection: close\r\n\r\n",
          'json_access_log_invalid_traceparent_result'
        )

        status = stop_process(wait_thread)
        lines = File.exist?(access_log_path) ? File.readlines(access_log_path, chomp: true) : []
        { exitstatus: status.exitstatus, response:, invalid_response:, lines:, output: "#{startup_output.join}#{output.read}" }
      ensure
        cleanup_process(wait_thread, output)
      end
    end
  end

  def custom_access_log_result
    Dir.mktmpdir('vajra-custom-access-log') do |root|
      access_log_path = File.join(root, 'access.log')
      script = <<~RUBY
        require "vajra"

        Vajra::Internal::RackExecution.install!(
          lambda do |_rack_env|
            [200, { "Content-Type" => "text/plain" }, ["OK"]]
          end
        )

        Vajra.start(
          workers: 1,
          threads: [1, 1],
          access_log: ENV.fetch("ACCESS_LOG_PATH"),
          access_log_format: "%m %U %u"
        )
      RUBY

      managed_popen2e(
        vajra_env(port: disposable_listener_port).merge('ACCESS_LOG_PATH' => access_log_path),
        *inline_ruby_command(script),
        chdir: VajraE2EHelpers::PACKAGE_ROOT
      ) do |_stdin, output, wait_thread|
        startup_output = []
        selected_port = wait_for_banner(output, captured_lines: startup_output)
        socket = TCPSocket.new(VajraE2EHelpers::LISTENER_HOST, selected_port)
        begin
          socket.write("GET /tokens HTTP/1.1\r\nHost: example.test\r\nUser-Agent: vajra\tagent\r\nConnection: close\r\n\r\n")
          response = parse_http_response(read_raw_http_response(socket, wait_thread:, output:, request_label: 'custom_access_log_result'))
        ensure
          socket.close unless socket.closed?
        end

        status = stop_process(wait_thread)
        lines = File.exist?(access_log_path) ? File.readlines(access_log_path, chomp: true) : []
        { exitstatus: status.exitstatus, response:, lines:, output: "#{startup_output.join}#{output.read}" }
      ensure
        cleanup_process(wait_thread, output)
      end
    end
  end

  def otel_observability_script
    <<~RUBY
      require "json"
      require "vajra"

      module Kernel
        alias vajra_original_require require
        def require(name)
          return true if name == "opentelemetry/sdk"

          vajra_original_require(name)
        end
      end

      module OpenTelemetry
        class << self
          attr_accessor :tracer_provider
        end

        def self.propagation
          @propagation ||= Class.new { def extract(carrier) = carrier }.new
        end

        module Trace
          class Status
            def self.ok = "ok"
            def self.error(message) = ["error", message]
          end
        end
      end

      class FakeSpanContext
        def hex_trace_id = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        def hex_span_id = "bbbbbbbbbbbbbbbb"
      end

      class FakeSpan
        attr_reader :attributes
        attr_accessor :status
        def initialize = @attributes = {}
        def context = FakeSpanContext.new
        def set_attribute(name, value) = @attributes[name] = value
        def record_exception(error) = @attributes["exception.message"] = error.message
      end

      class FakeTracer
        def sampled?(_carrier) = ENV.fetch("UNSAMPLED_OTEL", "") != "1"
        def in_span(name, **options)
          span = FakeSpan.new
          yield span
        ensure
          File.open(ENV.fetch("SPAN_LOG_PATH"), "a") do |file|
            file.puts(JSON.generate(name:, attributes: options.fetch(:attributes), status: span&.status))
          end
        end
      end

      OpenTelemetry.tracer_provider = Class.new { def tracer(_service_name) = FakeTracer.new }.new
      Vajra::Internal::RackExecution.install!(lambda { |_rack_env| [200, { "Content-Type" => "text/plain" }, ["OK"]] })
      Vajra.start(
        workers: 1,
        threads: [1, 1],
        access_log: ENV.fetch("ACCESS_LOG_PATH"),
        access_log_format: "json",
        log_level: ENV.fetch("UNSAMPLED_OTEL", "") == "1" ? "info" : "debug",
        structured_logs: true,
        stats_path: "/__vajra/stats",
        trace_enabled: true,
        trace_service_name: "vajra-e2e"
      )
    RUBY
  end

  def otel_observability_request(selected_port, wait_thread, output, request, label)
    socket = TCPSocket.new(VajraE2EHelpers::LISTENER_HOST, selected_port)
    socket.write(request)
    parse_http_response(read_raw_http_response(socket, wait_thread:, output:, request_label: label))
  ensure
    socket.close unless socket.nil? || socket.closed?
  end

  def otel_observability_result
    Dir.mktmpdir('vajra-otel-observability') do |root|
      access_log_path = File.join(root, 'access.log')
      span_log_path = File.join(root, 'spans.jsonl')
      otel_observability_result_from_paths(access_log_path, span_log_path)
    end
  end

  def unsampled_otel_observability_result
    Dir.mktmpdir('vajra-unsampled-otel-observability') do |root|
      access_log_path = File.join(root, 'access.log')
      span_log_path = File.join(root, 'spans.jsonl')
      otel_observability_result_from_paths(access_log_path, span_log_path, extra_env: { 'UNSAMPLED_OTEL' => '1' }, include_stats: true)
    end
  end

  def otel_observability_result_from_paths(access_log_path, span_log_path, extra_env: {}, include_stats: false)
    managed_popen2e(
      vajra_env(port: disposable_listener_port).merge('ACCESS_LOG_PATH' => access_log_path, 'SPAN_LOG_PATH' => span_log_path).merge(extra_env),
      *inline_ruby_command(otel_observability_script),
      chdir: VajraE2EHelpers::PACKAGE_ROOT
    ) do |_stdin, output, wait_thread|
      startup_output = []
      selected_port = wait_for_banner(output, captured_lines: startup_output)
      normal_response = otel_observability_request(
        selected_port,
        wait_thread,
        output,
        "GET /active-span HTTP/1.1\r\nHost: example.test\r\nConnection: close\r\n\r\n",
        'otel_observability_result:normal'
      )
      malformed_response = otel_observability_request(selected_port, wait_thread, output, "NOT_HTTP\r\n\r\n", 'otel_observability_result:malformed')
      drain_response = otel_observability_request(
        selected_port,
        wait_thread,
        output,
        "GET /drain-native-observability HTTP/1.1\r\nHost: example.test\r\nConnection: close\r\n\r\n",
        'otel_observability_result:drain'
      )
      wait_for_native_span_log(span_log_path) unless extra_env.fetch('UNSAMPLED_OTEL', '') == '1'
      stats_response = if include_stats
                         otel_observability_request(
                           selected_port,
                           wait_thread,
                           output,
                           "GET /__vajra/stats HTTP/1.1\r\nHost: example.test\r\nConnection: close\r\n\r\n",
                           'otel_observability_result:stats'
                         )
                       end
      status = stop_process(wait_thread)
      {
        exitstatus: status.exitstatus,
        normal_response:,
        malformed_response:,
        drain_response:,
        stats_response:,
        access_lines: File.exist?(access_log_path) ? File.readlines(access_log_path, chomp: true) : [],
        span_lines: File.exist?(span_log_path) ? File.readlines(span_log_path, chomp: true) : [],
        output: "#{startup_output.join}#{output.read}"
      }
    ensure
      cleanup_process(wait_thread, output)
    end
  end

  def wait_for_native_span_log(span_log_path)
    100.times do
      return if File.exist?(span_log_path) && File.read(span_log_path).include?('request_parse_error')

      sleep 0.01
    end
  end

  def keep_alive_post_sequence_result(script:, request_bodies:)
    managed_popen2e(
      vajra_env(port: disposable_listener_port),
      *inline_ruby_command(script),
      chdir: VajraE2EHelpers::PACKAGE_ROOT
    ) do |_stdin, output, wait_thread|
      startup_output = []
      selected_port = wait_for_banner(output, captured_lines: startup_output)
      socket = TCPSocket.new(VajraE2EHelpers::LISTENER_HOST, selected_port)

      begin
        responses = request_bodies.map.with_index do |body, index|
          socket.write(
            "POST / HTTP/1.1\r\n" \
            "Host: localhost\r\n" \
            "Content-Type: text/plain\r\n" \
            "Content-Length: #{body.bytesize}\r\n\r\n" \
            "#{body}"
          )
          parse_http_response(
            read_raw_http_response(
              socket,
              wait_thread:,
              output:,
              request_label: "keep_alive_post_sequence_result:#{index}"
            )
          )
        end
      ensure
        socket.close unless socket.closed?
      end

      status = stop_process(wait_thread)
      { exitstatus: status.exitstatus, responses:, output: "#{startup_output.join}#{output.read}" }
    ensure
      cleanup_process(wait_thread, output)
    end
  end

  def open_request_sockets(port, count)
    Array.new(count) { TCPSocket.new(VajraE2EHelpers::LISTENER_HOST, port) }
  end

  def write_staged_request_bodies(sockets, request_bodies)
    sockets[0].write(post_request(request_bodies[0]))
    sockets[1].write(post_request(request_bodies[1]))
    sleep 0.05
    sockets[2].write(post_request(request_bodies[2]))
    sleep 0.05
    sockets[3].write(post_request(request_bodies[3]))
  end

  def read_responses_from_sockets(sockets, wait_thread, output)
    sockets.each_with_index.map do |socket, index|
      read_raw_http_response(
        socket,
        wait_thread:,
        output:,
        request_label: "staged_request_result:#{index}"
      )
    end
  end

  def observability_control_plane_script(threads: [1, 1])
    <<~RUBY
      require "vajra"

      Vajra::Internal::RackExecution.install!(
        lambda do |_rack_env|
          [200, { "Content-Type" => "text/plain" }, ["OK"]]
        end
      )

      Vajra.start(
        workers: 1,
        threads: #{threads.inspect},
        stats_path: "/__vajra/stats",
        metrics_endpoint: "/metrics",
        trace_enabled: true,
        trace_endpoint: "http://127.0.0.1:4318/v1/traces",
        trace_service_name: "vajra-e2e"
      )
    RUBY
  end

  def staged_request_result(script:, env:, request_bodies:)
    managed_popen2e(
      vajra_env(port: disposable_listener_port).merge(env),
      *inline_ruby_command(script),
      chdir: VajraE2EHelpers::PACKAGE_ROOT
    ) do |_stdin, output, wait_thread|
      startup_output = []
      selected_port = wait_for_banner(output, captured_lines: startup_output)

      sockets = open_request_sockets(selected_port, request_bodies.length)

      begin
        write_staged_request_bodies(sockets, request_bodies)
        responses = read_responses_from_sockets(sockets, wait_thread, output)

        status = stop_process(wait_thread)
        { exitstatus: status.exitstatus, responses:, output: "#{startup_output.join}#{output.read}" }
      ensure
        sockets.each { |socket| socket.close unless socket.closed? }
      end
    ensure
      cleanup_process(wait_thread, output)
    end
  end

  it 'fails startup with actionable bind diagnostics and releases startup resources' do
    blocking_server = bind_port
    blocked_port = blocking_server.addr[1]

    begin
      failure = startup_failure(port: blocked_port)

      expect(failure).to match(
        exitstatus: be_positive,
        output: a_string_including(
          "Unable to start Vajra: listener bind failed for 0.0.0.0:#{blocked_port}"
        )
      )
      expect(failure[:output]).not_to include(listener_banner(blocked_port))
    ensure
      blocking_server.close
    end

    rebound_server = bind_port(port: blocked_port)
    rebound_server.close
  end

  it 'fails startup with actionable VAJRA_PORT validation errors' do
    failure = startup_failure_with_env('not-a-port')

    expect(failure).to match(
      exitstatus: be_positive,
      output: a_string_including('Unable to start Vajra: invalid VAJRA_PORT: not-a-port')
    )
    expect(failure[:output]).to include('Expected an integer between 0 and 65535')
    expect(failure[:output]).to include('Use 0 to request an ephemeral port')
  end

  it 'fails startup with actionable VAJRA_MAX_REQUEST_HEAD_BYTES validation errors' do
    failure = startup_failure_with_config_env('VAJRA_MAX_REQUEST_HEAD_BYTES' => '0')

    expect(failure).to match(
      exitstatus: be_positive,
      output: a_string_including(
        'Unable to start Vajra: invalid VAJRA_MAX_REQUEST_HEAD_BYTES: 0'
      )
    )
    expect(failure[:output]).to include('Expected an integer between 1 and 2147483647')
  end

  it 'fails startup with actionable VAJRA_MAX_KEEPALIVE_REQUESTS validation errors' do
    failure = startup_failure_with_config_env('VAJRA_MAX_KEEPALIVE_REQUESTS' => '-1')

    expect(failure).to match(
      exitstatus: be_positive,
      output: a_string_including(
        'Unable to start Vajra: invalid VAJRA_MAX_KEEPALIVE_REQUESTS: -1'
      )
    )
    expect(failure[:output]).to include('Expected an integer between 0 and 2147483647')
  end

  it 'fails startup with actionable Ruby option validation errors' do
    failure = startup_failure_with_inline_start('RUBY_PORT' => '-1')

    expect(failure).to match(
      exitstatus: be_positive,
      output: a_string_including('Unable to start Vajra: invalid port option: -1')
    )
    expect(failure[:output]).to include('Expected an integer between 0 and 65535')
  end

  it 'fails startup with actionable unknown Ruby option errors' do
    failure = startup_failure_with_inline_script(<<~RUBY)
      require "vajra"
      Vajra.start(potr: 3000)
    RUBY

    expect(failure).to match(
      exitstatus: be_positive,
      output: a_string_including('unknown start option: potr')
    )
  end

  it 'fails startup when TLS is enabled without certificate credentials' do
    failure = startup_failure_with_inline_script(<<~RUBY)
      require "vajra"
      Vajra.start(tls: true)
    RUBY

    expect(failure).to match(
      exitstatus: be_positive,
      output: a_string_including('Unable to start Vajra: tls requires tls_certificate and tls_private_key')
    )
  end

  it 'fails startup when h2 is advertised without HTTP/2 enabled' do
    Dir.mktmpdir('vajra-invalid-h2') do |root|
      cert_path, key_path = write_self_signed_certificate(root)
      failure = startup_failure_with_inline_script(
        <<~RUBY,
          require "vajra"
          Vajra.start(
            tls: true,
            tls_certificate: ENV.fetch("TLS_CERTIFICATE"),
            tls_private_key: ENV.fetch("TLS_PRIVATE_KEY"),
            alpn_protocols: ["h2", "http/1.1"]
          )
        RUBY
        env: {
          'TLS_CERTIFICATE' => cert_path,
          'TLS_PRIVATE_KEY' => key_path
        }
      )

      expect(failure).to match(
        exitstatus: be_positive,
        output: a_string_including('Unable to start Vajra: alpn_protocols cannot include h2 unless http2 is enabled')
      )
    end
  end

  it 'fails startup when Vajra.start is invoked from a non-main Ruby thread' do
    failure = startup_failure_with_inline_script(<<~RUBY)
      require "vajra"

      worker = Thread.new { Vajra.start }
      worker.join
    RUBY

    expect(failure).to match(
      exitstatus: be_positive,
      output: a_string_including(
        'Unable to start Vajra: worker-only Vajra.start must be invoked from the Ruby main thread'
      )
    )
  end

  it 'fails startup with actionable Ruby boot diagnostics' do
    failure = startup_failure_with_inline_script(<<~RUBY)
      require "vajra"

      Vajra::Internal::Boot.install!(lambda do |_boot_request|
        raise "boot exploded"
      end)

      Vajra.start
    RUBY

    expect(failure).to match(
      exitstatus: be_positive,
      output: a_string_including(
        'Unable to start Vajra: Ruby boot failed (boot_callback_error/boot): RuntimeError: boot exploded'
      )
    )
    expect(failure[:output]).not_to include('listening on port')
  end

  it 'fails startup with actionable worker bootstrap diagnostics' do
    failure = startup_failure_with_inline_script(<<~RUBY)
      require "vajra"

      Vajra::Internal::Boot.install!(lambda do |boot_request|
        next({ status: "ready", role: boot_request.fetch(:runtime_role) }) if boot_request.fetch(:runtime_role) == "ruby_master_preload"

        {
          status: "failed",
          role: boot_request.fetch(:runtime_role),
          diagnostic: {
            code: "worker_boot_failed",
            category: "boot",
            message: "worker activation exploded"
          }
        }
      end)

      Vajra.start
    RUBY

    expect(failure).to match(
      exitstatus: be_positive,
      output: a_string_including(
        'Unable to start Vajra: Ruby worker boot failed (worker_boot_failed/boot): worker activation exploded'
      )
    )
    expect(failure[:output]).not_to include('listening on port')
  end

  it 'preloads once in the master and exposes inherited state to the worker' do
    Dir.mktmpdir('vajra-preload-proof') do |dir|
      trace_path = File.join(dir, 'boot_trace.txt')

      script = <<~RUBY
        require "vajra"

        $vajra_preloaded_marker = nil

        Vajra::Internal::Boot.install!(lambda do |boot_request|
          File.open(ENV.fetch("BOOT_TRACE_PATH"), "a") do |file|
            file.puts(boot_request.fetch(:runtime_role))
          end

          case boot_request.fetch(:runtime_role)
          when "ruby_master_preload"
            $vajra_preloaded_marker = "preloaded-once"
          when "ruby_worker_bootstrap"
            raise "worker did not inherit preloaded marker" unless $vajra_preloaded_marker == "preloaded-once"
          end

          { status: "ready", role: boot_request.fetch(:runtime_role) }
        end)

        options = {}
        options[:port] = Integer(ENV["RUBY_PORT"]) if ENV.key?("RUBY_PORT")
        Vajra.start(**options)
      RUBY

      request = managed_popen2e(
        vajra_env.merge('RUBY_PORT' => disposable_listener_port.to_s, 'BOOT_TRACE_PATH' => trace_path),
        *inline_ruby_command(script),
        chdir: VajraE2EHelpers::PACKAGE_ROOT
      ) do |_stdin, output, wait_thread|
        startup_output = []
        selected_port = wait_for_banner(output, captured_lines: startup_output)

        socket = TCPSocket.new(VajraE2EHelpers::LISTENER_HOST, selected_port)
        socket.write("GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
        response = read_raw_http_response(socket, wait_thread:, output:, request_label: 'preload_inheritance')
        socket.close

        status = stop_process(wait_thread)

        { exitstatus: status.exitstatus, response:, port: selected_port, output: "#{startup_output.join}#{output.read}" }
      ensure
        cleanup_process(wait_thread, output)
      end

      expect(request[:exitstatus]).to eq(0), request[:output]
      expect(request[:response]).to include('HTTP/1.1 200 OK')
      expect(File.readlines(trace_path, chomp: true)).to eq(%w[ruby_master_preload ruby_worker_bootstrap])
    end
  end

  it 'lets Ruby configure the listener port when VAJRA_PORT is unset' do
    request = request_response_from_inline_start(env: { 'RUBY_PORT' => disposable_listener_port.to_s })

    expect(request[:exitstatus]).to eq(0)
    expect(request[:port]).to be_positive
    expect(request[:response]).to include('HTTP/1.1 200 OK')
  end

  it 'lets Ruby configure the listener host when VAJRA_HOST is unset' do
    request = request_response_from_inline_start(
      env: {
        'RUBY_HOST' => '127.0.0.1',
        'RUBY_PORT' => disposable_listener_port.to_s
      }
    )

    expect(request[:exitstatus]).to eq(0)
    expect(request[:port]).to be_positive
    expect(request[:response]).to include('HTTP/1.1 200 OK')
  end

  it 'starts the configured worker count and reports it in startup output' do
    request = request_response_from_inline_start(
      env: {
        'RUBY_HOST' => '127.0.0.1',
        'RUBY_PORT' => disposable_listener_port.to_s,
        'RUBY_WORKERS' => '4'
      }
    )

    expect(request[:exitstatus]).to eq(0)
    expect(request[:response]).to include('HTTP/1.1 200 OK')
    expect(request[:output]).to include(
      '* Workers: 4',
      '- Worker 0 (PID:',
      '- Worker 1 (PID:',
      '- Worker 2 (PID:',
      '- Worker 3 (PID:'
    )
  end

  it 'prefers WEB_CONCURRENCY over the Ruby worker option when VAJRA_WORKERS is unset' do
    request = request_response_from_inline_start(
      env: {
        'RUBY_HOST' => '127.0.0.1',
        'RUBY_PORT' => disposable_listener_port.to_s,
        'RUBY_WORKERS' => '2',
        'WEB_CONCURRENCY' => '3'
      }
    )

    expect(request[:exitstatus]).to eq(0)
    expect(request[:response]).to include('HTTP/1.1 200 OK')
    expect(request[:output]).to include('* Workers: 3')
    expect(request[:output]).to include('- Worker 2 (PID:')
  end

  it 'prefers MAX_THREADS over the Ruby max thread option and reports the effective range' do
    request = request_response_from_inline_start(
      env: {
        'RUBY_HOST' => '127.0.0.1',
        'RUBY_PORT' => disposable_listener_port.to_s,
        'RUBY_THREADS' => '2,4',
        'MAX_THREADS' => '6'
      }
    )

    expect(request[:exitstatus]).to eq(0)
    expect(request[:response]).to include('HTTP/1.1 200 OK')
    expect(request[:output]).to include(
      '* Min threads: 2',
      '* Max threads: 6'
    )
  end

  it 'fails startup with actionable thread-range validation errors' do
    failure = startup_failure_with_inline_start(
      'RUBY_PORT' => disposable_listener_port.to_s,
      'RUBY_THREADS' => '5,2'
    )

    expect(failure).to match(
      exitstatus: be_positive,
      output: a_string_including('Unable to start Vajra: invalid threads option: expected thread range with 1 <= min <= max')
    )
  end

  it 'fails startup with actionable MAX_THREADS validation errors' do
    failure = startup_failure_with_inline_start(
      'RUBY_PORT' => disposable_listener_port.to_s,
      'RUBY_THREADS' => '2,4',
      'MAX_THREADS' => '0'
    )

    expect(failure).to match(
      exitstatus: be_positive,
      output: a_string_including('Unable to start Vajra: invalid MAX_THREADS: 0')
    )
    expect(failure[:output]).to include('Expected an integer between 1 and 1024')
  end

  it 'fails startup with actionable request timeout validation errors' do
    failure = startup_failure_with_inline_start(
      'RUBY_PORT' => disposable_listener_port.to_s,
      'RUBY_REQUEST_TIMEOUT' => '0'
    )

    expect(failure).to match(
      exitstatus: be_positive,
      output: a_string_including('Unable to start Vajra: invalid request_timeout option: 0')
    )
    expect(failure[:output]).to include('Expected an integer between 1 and 2147483647')
  end

  it 'trims native string environment overrides before validation' do
    request = request_response_with_env(
      env: {
        'VAJRA_LOG_LEVEL' => " debug\t"
      }
    )

    expect(request[:exitstatus]).to eq(0)
    expect(request[:response]).to include('HTTP/1.1 200 OK')
  end

  it 'emits internal lifecycle boot events only when debug logging is enabled' do
    request = request_response_from_inline_start(
      env: {
        'RUBY_HOST' => '127.0.0.1',
        'RUBY_PORT' => disposable_listener_port.to_s,
        'RUBY_WORKERS' => '2',
        'RUBY_LOG_LEVEL' => 'debug'
      }
    )

    expect(request[:exitstatus]).to eq(0)
    expect(request[:output]).to include(
      'event=worker_registered',
      'event=worker_ready',
      'event=stop_completed'
    )
  end

  it 'boots Vajra and serves requests' do
    request = request_response_from_inline_start(
      env: {
        'RUBY_HOST' => '127.0.0.1',
        'RUBY_PORT' => disposable_listener_port.to_s
      }
    )

    expect(request[:exitstatus]).to eq(0), request[:output]
    expect(request[:response]).to include('HTTP/1.1 200 OK')
  end

  it 'serves HTTP/1.1 over TLS and exposes the HTTPS Rack scheme' do
    Dir.mktmpdir('vajra-tls') do |root|
      cert_path, key_path = write_self_signed_certificate(root)
      script = <<~RUBY
        require "vajra"

        Vajra::Internal::RackExecution.install!(
          lambda do |rack_env|
            [200, { "Content-Type" => "text/plain" }, [rack_env.fetch("rack.url_scheme")]]
          end
        )

        Vajra.start(
          workers: 1,
          threads: [1, 1],
          tls: true,
          tls_certificate: ENV.fetch("TLS_CERTIFICATE"),
          tls_private_key: ENV.fetch("TLS_PRIVATE_KEY"),
          tls_verify_mode: "none",
          alpn_protocols: ["http/1.1"]
        )
      RUBY

      result = tls_request_response(script:, cert_path:, key_path:)

      expect(result[:exitstatus]).to eq(0), result[:output]
      expect(result[:response]).to include('HTTP/1.1 200 OK')
      expect(result[:response]).to end_with('https')
    end
  end

  it 'serves a basic HTTP/2 request over TLS ALPN' do
    Dir.mktmpdir('vajra-h2') do |root|
      cert_path, key_path = write_self_signed_certificate(root)
      script = <<~RUBY
        require "vajra"

        Vajra::Internal::RackExecution.install!(
          lambda do |rack_env|
            [200, { "Content-Type" => "text/plain" }, ["h2:\#{rack_env.fetch("SERVER_PROTOCOL")}:\#{rack_env.fetch("rack.url_scheme")}"]]
          end
        )

        Vajra.start(
          workers: 1,
          threads: [1, 1],
          tls: true,
          tls_certificate: ENV.fetch("TLS_CERTIFICATE"),
          tls_private_key: ENV.fetch("TLS_PRIVATE_KEY"),
          tls_verify_mode: "none",
          alpn_protocols: ["h2", "http/1.1"],
          http2: true
        )
      RUBY

      result = h2_request_response(script:, cert_path:, key_path:)
      data_payloads = result[:frames].select { |frame| frame[:type] == 0 }.map { |frame| frame[:payload] }

      expect(result[:exitstatus]).to eq(0), result[:output]
      expect(data_payloads.join).to eq('h2:HTTP/2:https')
    end
  end

  it 'serves a real curl HTTP/2 request over TLS ALPN' do
    Dir.mktmpdir('vajra-curl-h2') do |root|
      cert_path, key_path = write_self_signed_certificate(root)
      script = <<~RUBY
        require "vajra"

        Vajra::Internal::RackExecution.install!(
          lambda do |rack_env|
            [200, { "Content-Type" => "text/plain" }, ["curl:\#{rack_env.fetch("SERVER_PROTOCOL")}:\#{rack_env.fetch("rack.url_scheme")}"]]
          end
        )

        Vajra.start(
          workers: 1,
          threads: [1, 1],
          tls: true,
          tls_certificate: ENV.fetch("TLS_CERTIFICATE"),
          tls_private_key: ENV.fetch("TLS_PRIVATE_KEY"),
          tls_verify_mode: "none",
          alpn_protocols: ["h2", "http/1.1"],
          http2: true
        )
      RUBY

      result = curl_http2_response(script:, cert_path:, key_path:)

      expect(result[:exitstatus]).to eq(0), result[:output]
      expect(result[:curl_exitstatus]).to eq(0), result[:stderr]
      expect(result[:stdout]).to include('curl:HTTP/2:https')
      expect(result[:stdout]).to end_with('2 200')
    end
  end

  it 'rejects strict HTTP/2 protocol errors with the expected error frames' do
    Dir.mktmpdir('vajra-h2-errors') do |root|
      cert_path, key_path = write_self_signed_certificate(root)
      script = <<~RUBY
        require "vajra"

        Vajra::Internal::RackExecution.install!(
          lambda do |_rack_env|
            [200, { "Content-Type" => "text/plain" }, ["ok"]]
          end
        )

        Vajra.start(
          workers: 1,
          threads: [4, 4],
          tls: true,
          tls_certificate: ENV.fetch("TLS_CERTIFICATE"),
          tls_private_key: ENV.fetch("TLS_PRIVATE_KEY"),
          tls_verify_mode: "none",
          alpn_protocols: ["h2", "http/1.1"],
          http2: true
        )
      RUBY

      result = h2_protocol_assertions(script:, cert_path:, key_path:) do |selected_port|
        socket = connected_tls_socket(selected_port, alpn_protocols: ['h2'])
        h2_start(socket)
        socket.write(
          h2_frame(1, 0x4, 1, h2_request_header_block) \
          + h2_frame(3, 0, 1, [0].pack('N')) \
          + h2_frame(0, 0x1, 1, 'late')
        )
        expect(h2_goaway_error_code(h2_read_until(socket, type: 7))).to eq(5)
        socket.close

        socket = connected_tls_socket(selected_port, alpn_protocols: ['h2'])
        h2_start(socket)
        socket.write(
          h2_frame(1, 0x5, 5, h2_request_header_block) \
          + h2_frame(1, 0x5, 3, h2_request_header_block)
        )
        expect(h2_goaway_error_code(h2_read_until(socket, type: 7))).to eq(1)
        socket.close

        socket = connected_tls_socket(selected_port, alpn_protocols: ['h2'])
        h2_start(socket)
        socket.write(h2_frame(2, 0, 1, [1, 16].pack('NC')))
        expect(h2_rst_stream_error_code(h2_read_until(socket, type: 3))).to eq(1)
        socket.close

        socket = connected_tls_socket(selected_port, alpn_protocols: ['h2'])
        h2_start(socket)
        socket.write(
          h2_frame(1, 0x4, 1, h2_request_header_block) \
          + h2_frame(8, 0, 1, [0x7fff_ffff].pack('N')) \
          + h2_frame(8, 0, 1, [0x7fff_ffff].pack('N'))
        )
        expect(h2_rst_stream_error_code(h2_read_until(socket, type: 3))).to eq(3)
        socket.close
      end

      expect(result[:exitstatus]).to eq(0), result[:output]
    end
  end

  it 'applies the configured request head timeout to fragmented request headers' do
    result = fragmented_request_result(
      chunks: [
        "GET /fragmented HTTP/1.1\r\nHost: localhost\r\nConnection: close\r",
        "\n",
        "\r",
        "\n"
      ],
      env: { 'VAJRA_REQUEST_HEAD_TIMEOUT' => '1' },
      pause: 1.2
    )

    expect(result[:exitstatus]).to eq(0)
    expect(result[:response]).to eq('')
    expect(result[:output]).not_to include('HTTP/1.1 200 OK')
  end

  it 'applies the configured first data timeout before the request head starts' do
    result = delayed_request_result(
      request: "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n",
      initial_pause: 1.2,
      env: { 'VAJRA_FIRST_DATA_TIMEOUT' => '1' }
    )

    expect(result[:exitstatus]).to eq(0)
    expect(result[:response]).to eq('')
    expect(result[:output]).not_to include('HTTP/1.1 200 OK')
  end

  it 'applies the configured persistent timeout between keepalive requests' do
    result = idle_keep_alive_timeout_result(env: { 'VAJRA_PERSISTENT_TIMEOUT' => '1' })

    expect(result[:exitstatus]).to eq(0)
    expect(result[:response]).to include(
      status_line: 'HTTP/1.1 200 OK',
      body: 'OK'
    )
    expect(result[:connection_closed]).to be(true)
    expect(result[:trailing_bytes]).to eq('')
  end

  it 'applies the configured max keepalive request cap' do
    result = max_keepalive_requests_result(env: { 'VAJRA_MAX_KEEPALIVE_REQUESTS' => '2' })

    expect(result[:exitstatus]).to eq(0), result[:output]
    expect(result[:first_response]).to include(status_line: 'HTTP/1.1 200 OK')
    expect(result[:second_response]).to include(status_line: 'HTTP/1.1 200 OK')
    expect(result[:connection_closed]).to be(true)
    expect(result[:trailing_bytes]).to eq('')
  end

  it 'prefers VAJRA_PORT over the Ruby port option even when the Ruby port would conflict' do
    blocking_server = nil
    blocking_server = bind_port
    ruby_port = blocking_server.addr[1]

    request = request_response_from_inline_start(
      env: { 'RUBY_PORT' => ruby_port.to_s, 'VAJRA_PORT' => disposable_listener_port.to_s }
    )

    expect(request[:exitstatus]).to eq(0)
    expect(request[:port]).to be_positive
    expect(request[:port]).not_to eq(ruby_port)
    expect(request[:response]).to include('HTTP/1.1 200 OK')
  ensure
    blocking_server&.close
  end

  it 'lets Ruby configure max_request_head_bytes when the env variable is unset' do
    result = oversized_request_result(
      env: {
        'VAJRA_PORT' => disposable_listener_port.to_s,
        'RUBY_MAX_REQUEST_HEAD_BYTES' => (32 * 1024).to_s
      },
      payload_size: 20 * 1024
    )

    expect(result[:exitstatus]).to eq(0)
    expect(result[:response]).to include('HTTP/1.1 200 OK')
  end

  it 'prefers VAJRA_MAX_REQUEST_HEAD_BYTES over the Ruby max_request_head_bytes option' do
    result = oversized_request_result(
      env: {
        'VAJRA_PORT' => disposable_listener_port.to_s,
        'VAJRA_MAX_REQUEST_HEAD_BYTES' => '128',
        'RUBY_MAX_REQUEST_HEAD_BYTES' => (64 * 1024).to_s
      },
      payload_size: 512
    )

    expect(result[:exitstatus]).to eq(0)
    expect(result[:response]).to include('HTTP/1.1 431 Request Header Fields Too Large')
    expect(result[:output]).to include('request rejected (431 request header fields too large)')
  end

  it 'serves the configured stats path from the native control plane' do
    result = control_plane_response_result(script: observability_control_plane_script, path: '/__vajra/stats')

    expect(result[:exitstatus]).to eq(0), result[:output]
    expect(result[:response][:status_line]).to eq('HTTP/1.1 200 OK')
    expect(result[:response][:headers]).to include('content-type' => 'application/json')
    payload = JSON.parse(result[:response][:body])
    expect(payload).to include('master_pid', 'master_rss_bytes', 'profiling', 'socket_queue_capacity')
    expect(payload).to have_key('workers')
    expect(payload.fetch('workers').first).to include(
      'pid',
      'rss_bytes',
      'unexpected_exit_count',
      'recovery_deadline_nanoseconds'
    )
  end

  it 'keeps the connection alive for repeated post requests with a request body' do
    result = keep_alive_post_sequence_result(
      script: <<~RUBY,
        require "vajra"

        Vajra::Internal::RackExecution.install!(
          lambda do |rack_env|
            body = rack_env.fetch("rack.input").read
            [200, { "Content-Type" => "text/plain" }, [body.upcase]]
          end
        )

        Vajra.start(workers: 1, threads: [1, 1])
      RUBY
      request_bodies: %w[first second]
    )

    expect(result[:exitstatus]).to eq(0), result[:output]
    expect(result[:responses].size).to eq(2)
    expect(result[:responses].map { |response| response[:status_line] }).to eq(['HTTP/1.1 200 OK', 'HTTP/1.1 200 OK'])
    expect(result[:responses].map { |response| response[:body] }).to eq(%w[FIRST SECOND])
    expect(result[:responses].all? { |response| response[:headers]['connection'] != 'close' }).to be(true)
  end

  it 'exposes worker execution capacity when max threads exceed min threads' do
    result = control_plane_response_result(
      script: observability_control_plane_script(threads: [1, 2]),
      path: '/__vajra/stats'
    )

    expect(result[:exitstatus]).to eq(0), result[:output]
    payload = JSON.parse(result[:response][:body])
    expect(payload.fetch('workers').first).to include(
      'active_execution_count' => 0,
      'idle_execution_count' => 2
    )
  end

  it 'serves the configured metrics endpoint from the native control plane' do
    result = control_plane_response_result(script: observability_control_plane_script, path: '/metrics')

    expect(result[:exitstatus]).to eq(0), result[:output]
    expect(result[:response][:status_line]).to eq('HTTP/1.1 200 OK')
    expect(result[:response][:headers]).to include('content-type' => 'text/plain; version=0.0.4')
    expect(result[:response][:body]).to include(
      'vajra_worker_active_connections',
      'vajra_worker_active_executions',
      'vajra_worker_idle_executions',
      'vajra_worker_accept_total'
    )
  end

  it 'emits structured access logs with request metadata and trace correlation' do
    result = json_access_log_result

    expect(result[:exitstatus]).to eq(0), result[:output]
    expect(result[:response][:status_line]).to eq('HTTP/1.1 200 OK')
    access_event = JSON.parse(result[:lines].find { |line| line.include?('"component":"access"') })
    expect(access_event).to include(
      'component' => 'access',
      'method' => 'GET',
      'target' => '/observability',
      'status' => 200,
      'bytes_written' => 2,
      'remote_addr' => VajraE2EHelpers::LISTENER_HOST,
      'protocol' => 'HTTP/1.1',
      'host' => 'example.test',
      'user_agent' => 'vajra-e2e',
      'referer' => 'https://example.test/source',
      'request_id' => 'request-123',
      'connection_outcome' => 'close',
      'trace_id' => '11111111111111111111111111111111',
      'span_id' => '2222222222222222'
    )
    expect(access_event.fetch('duration_nanoseconds')).to be > 0
    expect(access_event.fetch('worker_pid')).to be > 0
    expect(access_event.fetch('worker_index')).to eq(0)
  end

  it 'does not use malformed traceparent values for structured access-log correlation' do
    result = json_access_log_result

    expect(result[:exitstatus]).to eq(0), result[:output]
    expect(result[:invalid_response][:status_line]).to eq('HTTP/1.1 200 OK')
    access_event = JSON.parse(result[:lines].find { |line| line.include?('/invalid-traceparent') })
    expect(access_event).to include(
      'component' => 'access',
      'target' => '/invalid-traceparent',
      'status' => 200
    )
    expect(access_event).not_to include('trace_id', 'span_id')
  end

  it 'escapes request-derived values in custom access log formats' do
    result = custom_access_log_result

    expect(result[:exitstatus]).to eq(0), result[:output]
    expect(result[:response][:status_line]).to eq('HTTP/1.1 200 OK')
    expect(result[:lines]).to include('GET /tokens vajra\\tagent')
  end

  it 'correlates access logs with active OTel span ids and emits native error spans' do
    result = otel_observability_result

    expect(result[:exitstatus]).to eq(0), result[:output]
    expect(result[:normal_response][:status_line]).to eq('HTTP/1.1 200 OK')
    expect(result[:normal_response][:headers]).not_to include(
      'x-vajra-internal-trace-id',
      'x-vajra-internal-span-id'
    )
    expect(result[:malformed_response][:status_line]).to eq('HTTP/1.1 400 Bad Request')

    access_event = JSON.parse(result[:access_lines].find { |line| line.include?('/active-span') })
    expect(access_event).to include(
      'trace_id' => 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',
      'span_id' => 'bbbbbbbbbbbbbbbb'
    )

    spans = result[:span_lines].map { |line| JSON.parse(line) }
    lifecycle_span = spans.find { |span| span.fetch('name') == 'vajra.worker_ready' }
    expect(lifecycle_span).not_to be_nil
    expect(lifecycle_span.fetch('attributes')).to include(
      'vajra.worker.index' => 0,
      'vajra.worker.lifecycle_state' => 'ready',
      'vajra.worker.health_state' => 'healthy'
    )

    native_error_span = spans.find do |span|
      span.fetch('attributes')['vajra.request.outcome'] == 'request_parse_error'
    end
    expect(native_error_span).not_to be_nil
    expect(native_error_span.fetch('attributes')).to include(
      'http.response.status_code' => 400,
      'vajra.response.sent' => true
    )
    expect(native_error_span.fetch('status').first).to eq('error')
  end

  it 'skips unsampled OTel spans while preserving logs and native stats' do
    result = unsampled_otel_observability_result

    expect(result[:exitstatus]).to eq(0), result[:output]
    expect(result[:normal_response][:status_line]).to eq('HTTP/1.1 200 OK')
    expect(result[:malformed_response][:status_line]).to eq('HTTP/1.1 400 Bad Request')
    expect(result[:span_lines]).to be_empty

    access_event = JSON.parse(result[:access_lines].find { |line| line.include?('/active-span') })
    expect(access_event).to include('status' => 200)
    expect(access_event).not_to include('trace_id', 'span_id')

    stats = JSON.parse(result[:stats_response][:body])
    expect(stats.fetch('native_observability')).to include(
      'request_events_total' => be >= 1,
      'request_errors_total' => be >= 1
    )
  end
end
