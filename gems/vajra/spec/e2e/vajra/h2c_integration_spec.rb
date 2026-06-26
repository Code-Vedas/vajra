# frozen_string_literal: true

# Copyright Codevedas Inc. 2025-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

require_relative 'support'
require 'base64'
require 'fileutils'
require 'openssl'
require 'tmpdir'

RSpec.describe 'Vajra h2c integration', :e2e, :integration do
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
    payload = length.zero? ? ''.b : socket.read(length)
    { length:, type:, flags:, stream_id: stream_id & 0x7fff_ffff, payload: }
  end

  def h2_request_header_block(path)
    header_block = +"\x82".b
    header_block << (path == '/' ? "\x84".b : "\x44".b + h2_string(path))
    header_block << "\x86".b
    header_block << "\x41".b << h2_string('localhost')
    header_block
  end

  def h2_literal_header(name, value)
    "\x00".b + h2_string(name) + h2_string(value)
  end

  def h2_headers(*pairs)
    pairs.each_with_object(+''.b) do |(name, value), block|
      block << h2_literal_header(name, value)
    end
  end

  def h2_read_response_body(socket, stream_id)
    body = +''.b
    Timeout.timeout(5) do
      loop do
        frame = h2_read_frame(socket)
        h2_ack_settings(socket, frame)
        if frame[:type].zero? && frame[:stream_id] == stream_id
          body << frame[:payload]
          h2_window_update(socket, 0, frame[:payload].bytesize)
          h2_window_update(socket, stream_id, frame[:payload].bytesize)
        end
        break if frame[:type].zero? && frame[:stream_id] == stream_id && frame[:flags].anybits?(0x1)
      end
    end
    body
  end

  def h2_window_update(socket, stream_id, increment)
    return if increment.zero?

    socket.write(h2_frame(8, 0, stream_id, [increment].pack('N')))
  end

  def h2_ack_settings(socket, frame)
    socket.write(h2_frame(4, 0x1, 0, '')) if frame[:type] == 4 && frame[:flags].nobits?(0x1)
  end

  def h2_priority_spec(dependency_stream_id, weight, exclusive: false)
    dependency = dependency_stream_id & 0x7fff_ffff
    dependency |= 0x8000_0000 if exclusive
    [dependency, weight - 1].pack('NC')
  end

  def h2_priority_frame(stream_id, dependency_stream_id, weight, exclusive: false)
    h2_frame(2, 0, stream_id, h2_priority_spec(dependency_stream_id, weight, exclusive:))
  end

  def h2_start(socket, settings_payload: '')
    socket.write("PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n#{h2_frame(4, 0, 0, settings_payload)}")
    Timeout.timeout(5) do
      loop do
        frame = h2_read_frame(socket)
        h2_ack_settings(socket, frame)
        return if frame[:type] == 4 && frame[:flags].nobits?(0x1)
      end
    end
  end

  def h2_wait_for_settings_ack(socket)
    Timeout.timeout(5) do
      loop do
        frame = h2_read_frame(socket)
        h2_ack_settings(socket, frame)
        return if frame[:type] == 4 && frame[:flags].anybits?(0x1)
      end
    end
  end

  def h2_read_until(socket, type:, stream_id: nil)
    Timeout.timeout(5) do
      loop do
        frame = h2_read_frame(socket)
        h2_ack_settings(socket, frame)
        next unless frame[:type] == type
        next if stream_id && frame[:stream_id] != stream_id

        return frame
      end
    end
  end

  def h2_wait_for_response_headers(socket, stream_ids)
    pending = stream_ids.dup
    Timeout.timeout(5) do
      until pending.empty?
        frame = h2_read_frame(socket)
        h2_ack_settings(socket, frame)
        pending.delete(frame[:stream_id]) if frame[:type] == 1
        raise "stream #{frame[:stream_id]} reset while waiting for response headers" if frame[:type] == 3 && pending.include?(frame[:stream_id])
      end
    end
  end

  def h2_release_response_windows(socket, stream_ids, bytes: 400 * 1024)
    h2_window_update(socket, 0, bytes)
    stream_ids.each { |stream_id| h2_window_update(socket, stream_id, bytes) }
  end

  def h2_first_data_stream(socket)
    Timeout.timeout(5) do
      loop do
        frame = h2_read_frame(socket)
        h2_ack_settings(socket, frame)
        return frame[:stream_id] if frame[:type].zero?
      end
    end
  end

  def h2_rst_stream_error_code(frame)
    frame.fetch(:payload).unpack1('N')
  end

  def build_self_signed_certificate(key)
    cert = OpenSSL::X509::Certificate.new
    cert.version = 2
    cert.serial = 1
    cert.subject = OpenSSL::X509::Name.parse('/CN=localhost')
    cert.issuer = cert.subject
    cert.public_key = key.public_key
    cert.not_before = Time.now.utc - 60
    cert.not_after = Time.now.utc + 3600
    add_self_signed_extensions(cert)
    cert.sign(key, OpenSSL::Digest.new('SHA256'))
    cert
  end

  def add_self_signed_extensions(cert)
    extension_factory = OpenSSL::X509::ExtensionFactory.new
    extension_factory.subject_certificate = cert
    extension_factory.issuer_certificate = cert
    cert.add_extension(extension_factory.create_extension('subjectAltName', 'DNS:localhost,IP:127.0.0.1'))
    cert.add_extension(extension_factory.create_extension('basicConstraints', 'CA:FALSE', true))
  end

  def self_signed_certificate(root)
    key = OpenSSL::PKey::RSA.new(2048)
    cert = build_self_signed_certificate(key)
    cert_path = File.join(root, 'cert.pem')
    key_path = File.join(root, 'key.pem')
    File.write(cert_path, cert.to_pem)
    File.write(key_path, key.to_pem)
    [cert_path, key_path]
  end

  def connected_tls_h2_socket(port)
    tcp_socket = TCPSocket.new(VajraE2EHelpers::LISTENER_HOST, port)
    context = OpenSSL::SSL::SSLContext.new
    context.verify_mode = OpenSSL::SSL::VERIFY_NONE
    context.alpn_protocols = ['h2']
    ssl_socket = OpenSSL::SSL::SSLSocket.new(tcp_socket, context)
    ssl_socket.hostname = 'localhost'
    ssl_socket.connect
    raise "unexpected ALPN protocol: #{ssl_socket.alpn_protocol.inspect}" unless ssl_socket.alpn_protocol == 'h2'

    ssl_socket
  end

  def h2c_server_result(http2: true)
    script = <<~RUBY
      require "vajra"

      Vajra::Internal::RackExecution.install!(
        lambda do |rack_env|
          body = [
            rack_env.fetch("REQUEST_METHOD"),
            rack_env.fetch("PATH_INFO"),
            rack_env.fetch("SERVER_PROTOCOL"),
            rack_env.fetch("rack.url_scheme")
          ].join(":")
          [200, { "Content-Type" => "text/plain" }, [body]]
        end
      )

      Vajra.start(workers: 1, threads: [1, 1], http2: #{http2})
    RUBY

    managed_popen2e(
      vajra_env(port: disposable_listener_port),
      *inline_ruby_command(script),
      chdir: VajraE2EHelpers::PACKAGE_ROOT
    ) do |_stdin, output, wait_thread|
      startup_output = []
      selected_port = wait_for_banner(output, captured_lines: startup_output)
      begin
        yield selected_port
      rescue StandardError => e
        runtime_output = read_available_output(output)
        raise "#{e.class}: #{e.message}\n#{startup_output.join}#{runtime_output}"
      end
      status = stop_process(wait_thread)
      { exitstatus: status.exitstatus, output: "#{startup_output.join}#{output.read}" }
    ensure
      cleanup_process(wait_thread, output)
    end
  end

  def h2c_tunnel_server_script(tls:, force_active_tracing: false)
    <<~RUBY
      require "vajra"

      if #{force_active_tracing}
        Vajra::Internal::Tracing.send(
          :__native_set_tracing_status__,
          true,
          true,
          "",
          "vajra-test",
          true,
          1.0,
          "",
          "tracecontext"
        )
      end

      Vajra::Internal::RackExecution.install!(
        lambda do |rack_env|
          stream = rack_env["vajra.http2.stream"]
          unless stream
            next [404, { "Content-Type" => "text/plain" }, ["no stream"]]
          end

          if rack_env["PATH_INFO"] == "/reset"
            stream.accept(200, "content-type" => "application/octet-stream")
            stream.reset(8)
            next [200, {}, []]
          end

          if rack_env["PATH_INFO"] == "/normal"
            next [202, { "Content-Type" => "text/plain" }, ["normal-connect-response"]]
          end

          if rack_env["PATH_INFO"] == "/double-accept"
            stream.accept(200, "content-type" => "text/plain")
            message = begin
              stream.accept(200, {})
              "no-error"
            rescue IOError => e
              e.message
            end
            stream.write(message)
            stream.close
            next [200, {}, []]
          end

          prefix =
            if rack_env["vajra.http2.websocket"]
              "ws:"
            else
              "tunnel:"
            end
          stream.accept(200, "content-type" => "application/octet-stream")
          payload = stream.read.to_s
          stream.write(prefix + payload)
          stream.close
          [200, {}, []]
        end
      )

      options = {
        workers: 1,
        threads: [1, 1],
        http2: true
      }
      if #{tls}
        options.merge!(
          tls: true,
          tls_certificate: ENV.fetch("TLS_CERTIFICATE"),
          tls_private_key: ENV.fetch("TLS_PRIVATE_KEY"),
          tls_verify_mode: "none",
          alpn_protocols: ["h2", "http/1.1"]
        )
      end
      Vajra.start(**options)
    RUBY
  end

  def h2c_tunnel_env(tls)
    env = vajra_env(port: disposable_listener_port)
    root = nil
    if tls
      root = Dir.mktmpdir('vajra-h2-tunnel')
      cert_path, key_path = self_signed_certificate(root)
      env = env.merge('TLS_CERTIFICATE' => cert_path, 'TLS_PRIVATE_KEY' => key_path)
    end
    [env, root]
  end

  def h2c_tunnel_server_result(tls: false, force_active_tracing: false)
    env, root = h2c_tunnel_env(tls)
    managed_popen2e(
      env,
      *inline_ruby_command(h2c_tunnel_server_script(tls:, force_active_tracing:)),
      chdir: VajraE2EHelpers::PACKAGE_ROOT
    ) do |_stdin, output, wait_thread|
      startup_output = []
      selected_port = wait_for_banner(output, captured_lines: startup_output)
      begin
        yield selected_port
      rescue StandardError => e
        runtime_output = read_available_output(output)
        raise "#{e.class}: #{e.message}\n#{startup_output.join}#{runtime_output}"
      end
      status = stop_process(wait_thread)
      { exitstatus: status.exitstatus, output: "#{startup_output.join}#{output.read}" }
    ensure
      cleanup_process(wait_thread, output)
      FileUtils.remove_entry(root) if tls && root
    end
  end

  def h2_large_response_server_script(tls:)
    <<~RUBY
      require "vajra"

      body = ("a" * (300 * 1024)) + ("b" * 1024)
      Vajra::Internal::RackExecution.install!(
        lambda do |_rack_env|
          [200, { "Content-Type" => "application/octet-stream" }, [body]]
        end
      )

      options = {
        workers: 1,
        threads: [1, 1],
        http2: true
      }
      if #{tls}
        options.merge!(
          tls: true,
          tls_certificate: ENV.fetch("TLS_CERTIFICATE"),
          tls_private_key: ENV.fetch("TLS_PRIVATE_KEY"),
          tls_verify_mode: "none",
          alpn_protocols: ["h2", "http/1.1"]
        )
      end
      Vajra.start(**options)
    RUBY
  end

  def h2_large_response_result(tls:, staged_window_update: false)
    env, root = h2c_tunnel_env(tls)
    managed_popen2e(
      env,
      *inline_ruby_command(h2_large_response_server_script(tls:)),
      chdir: VajraE2EHelpers::PACKAGE_ROOT
    ) do |_stdin, output, wait_thread|
      startup_output = []
      selected_port = wait_for_banner(output, captured_lines: startup_output)
      begin
        body = if staged_window_update
                 read_h2_large_response_body_after_deferred_window_update(selected_port, tls:)
               else
                 read_h2_large_response_body(selected_port, tls:)
               end
        { body: }
      rescue StandardError => e
        runtime_output = read_available_output(output)
        raise "#{e.class}: #{e.message}\n#{startup_output.join}#{runtime_output}"
      end.then do |payload|
        status = stop_process(wait_thread)
        payload.merge(exitstatus: status.exitstatus, output: "#{startup_output.join}#{output.read}")
      end
    ensure
      cleanup_process(wait_thread, output)
      FileUtils.remove_entry(root) if tls && root
    end
  end

  def read_h2_large_response_body(selected_port, tls:)
    socket = connected_h2_socket(selected_port, tls:)
    h2_start(socket)
    socket.write(h2_frame(1, 0x5, 1, h2_request_header_block('/large')))
    h2_read_response_body(socket, 1)
  ensure
    socket&.close unless socket.nil? || socket.closed?
  end

  def read_h2_large_response_body_after_deferred_window_update(selected_port, tls:)
    socket = connected_h2_socket(selected_port, tls:)
    h2_start(socket, settings_payload: [4, 1024].pack('nN'))
    socket.write(h2_frame(1, 0x5, 1, h2_request_header_block('/large')))
    body = +''.b
    released_window = false
    Timeout.timeout(5) do
      loop do
        frame = h2_read_frame(socket)
        h2_ack_settings(socket, frame)
        next unless frame[:type].zero? && frame[:stream_id] == 1

        body << frame[:payload]
        if released_window
          h2_extend_large_response_window(socket, 1, frame)
        else
          h2_release_large_response_window(socket, 1)
          released_window = true
        end
        break if frame[:flags].anybits?(0x1)
      end
    end
    raise 'response did not reach the constrained flow-control window' unless released_window

    body
  ensure
    socket&.close unless socket.nil? || socket.closed?
  end

  def h2_extend_large_response_window(socket, stream_id, frame)
    h2_window_update(socket, 0, frame[:payload].bytesize)
    h2_window_update(socket, stream_id, frame[:payload].bytesize)
  end

  def h2_release_large_response_window(socket, stream_id)
    h2_window_update(socket, 0, 400 * 1024)
    h2_window_update(socket, stream_id, 400 * 1024)
  end

  def h2c_priority_server_result(expected_paths: ['/high', '/low'])
    script = <<~RUBY
      require "vajra"

      expected_paths = #{expected_paths.inspect}
      priority_mutex = Mutex.new
      priority_condition = ConditionVariable.new
      priority_seen = {}

      priority_body = lambda do |path|
        priority_mutex.synchronize do
          priority_seen[path] = true
          priority_condition.broadcast
          deadline = Process.clock_gettime(Process::CLOCK_MONOTONIC) + 5.0
          until expected_paths.all? { |expected| priority_seen[expected] } || Process.clock_gettime(Process::CLOCK_MONOTONIC) >= deadline
            priority_condition.wait(priority_mutex, 0.05)
          end
        end

        case path
        when "/high" then "H" * 65_536
        when "/mid" then "M" * 65_536
        when "/low" then "L" * 65_536
        else "ok"
        end
      end

      Vajra::Internal::RackExecution.install!(
        lambda do |rack_env|
          body = priority_body.call(rack_env["PATH_INFO"])
          [200, { "Content-Type" => "text/plain" }, [body]]
        end
      )

      Vajra.start(workers: 1, threads: [4, 4], http2: true)
    RUBY

    managed_popen2e(
      vajra_env(port: disposable_listener_port),
      *inline_ruby_command(script),
      chdir: VajraE2EHelpers::PACKAGE_ROOT
    ) do |_stdin, output, wait_thread|
      startup_output = []
      selected_port = wait_for_banner(output, captured_lines: startup_output)
      begin
        yield selected_port
      rescue StandardError => e
        runtime_output = read_available_output(output)
        raise "#{e.class}: #{e.message}\n#{startup_output.join}#{runtime_output}"
      end
      status = stop_process(wait_thread)
      { exitstatus: status.exitstatus, output: "#{startup_output.join}#{output.read}" }
    ensure
      cleanup_process(wait_thread, output)
    end
  end

  def invalid_extended_connect_header_blocks
    [
      h2_headers([':method', 'CONNECT'], [':scheme', 'http'], [':authority', 'localhost'], [':path', '/missing-protocol']),
      h2_headers([':method', 'GET'], [':scheme', 'http'], [':authority', 'localhost'], [':path', '/not-connect'], [':protocol', 'websocket']),
      h2_headers([':method', 'CONNECT'], [':authority', 'localhost'], [':path', '/missing-scheme'], [':protocol', 'echo']),
      h2_headers([':method', 'CONNECT'], [':scheme', 'http'], [':authority', 'localhost'], [':protocol', 'echo']),
      h2_headers([':method', 'CONNECT'], [':scheme', 'http'], [':path', '/missing-authority'], [':protocol', 'echo']),
      h2_headers(
        [':method', 'CONNECT'],
        [':scheme', 'http'],
        [':authority', 'localhost'],
        [':path', '/connection-header'],
        [':protocol', 'echo'],
        %w[connection keep-alive]
      )
    ]
  end

  def connected_plain_socket(port)
    TCPSocket.new(VajraE2EHelpers::LISTENER_HOST, port)
  end

  def connected_h2_socket(port, tls:)
    tls ? connected_tls_h2_socket(port) : connected_plain_socket(port)
  end

  def h2_tunnel_echo_headers(path: '/tunnel', protocol: 'echo')
    h2_headers(
      [':method', 'CONNECT'],
      [':scheme', 'http'],
      [':authority', 'localhost'],
      [':path', path],
      [':protocol', protocol]
    )
  end

  def h2_expect_stream_reset(port, header_block, tls: false)
    socket = connected_h2_socket(port, tls:)
    h2_start(socket)
    socket.write(h2_frame(1, 0x5, 1, header_block))
    reset_frame = h2_read_until(socket, type: 3, stream_id: 1)
    expect(h2_rst_stream_error_code(reset_frame)).to eq(1)
    socket.close
  end

  def h2_tunnel_echo_result(tls:, websocket: false, force_active_tracing: false)
    h2c_tunnel_server_result(tls:, force_active_tracing:) do |port|
      socket = connected_h2_socket(port, tls:)
      h2_start(socket)
      payload = websocket ? "\x81\x02hi".b : 'ping'
      headers = h2_tunnel_echo_headers(
        path: websocket ? '/websocket' : '/tunnel',
        protocol: websocket ? 'websocket' : 'echo'
      )
      socket.write(
        "#{h2_frame(1, 0x4, 1, headers)}" \
        "#{h2_frame(0, 0x1, 1, payload)}"
      )
      prefix = websocket ? 'ws:' : 'tunnel:'
      expect(h2_read_response_body(socket, 1)).to eq("#{prefix}#{payload}")
      socket.close
    end
  end

  def expect_bad_h2c_upgrade(request_head)
    result = h2c_server_result do |port|
      socket = connected_plain_socket(port)
      socket.write(request_head)
      response = socket.readpartial(128)
      expect(response).to start_with('HTTP/1.1 400 Bad Request')
      socket.close
    end

    expect(result[:exitstatus]).to eq(0), result[:output]
  end

  it 'serves a cleartext prior-knowledge HTTP/2 request when http2 is enabled' do
    result = h2c_server_result do |port|
      socket = connected_plain_socket(port)
      socket.write(
        "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n" \
        "#{h2_frame(4, 0, 0, '')}" \
        "#{h2_frame(1, 0x5, 1, h2_request_header_block('/'))}"
      )
      expect(h2_read_response_body(socket, 1)).to eq('GET:/:HTTP/2:http')
      socket.close
    end

    expect(result[:exitstatus]).to eq(0), result[:output]
  end

  it 'serves a fragmented cleartext prior-knowledge preface' do
    result = h2c_server_result do |port|
      socket = connected_plain_socket(port)
      socket.write('PRI *')
      sleep 0.05
      socket.write(
        " HTTP/2.0\r\n\r\nSM\r\n\r\n" \
        "#{h2_frame(4, 0, 0, '')}" \
        "#{h2_frame(1, 0x5, 1, h2_request_header_block('/fragmented'))}"
      )
      expect(h2_read_response_body(socket, 1)).to eq('GET:/fragmented:HTTP/2:http')
      socket.close
    end

    expect(result[:exitstatus]).to eq(0), result[:output]
  end

  it 'rejects cleartext prior knowledge when http2 is disabled' do
    result = h2c_server_result(http2: false) do |port|
      socket = connected_plain_socket(port)
      socket.write("PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n")
      response = socket.readpartial(128)
      expect(response).to start_with('HTTP/1.1 400 Bad Request')
      socket.close
    end

    expect(result[:exitstatus]).to eq(0), result[:output]
  end

  it 'rejects invalid HTTP/1.1 h2c upgrade requests' do
    result = h2c_server_result do |port|
      socket = connected_plain_socket(port)
      socket.write(
        "GET /upgrade HTTP/1.1\r\n" \
        "Host: localhost\r\n" \
        "Connection: Upgrade\r\n" \
        "Upgrade: h2c\r\n" \
        "\r\n"
      )
      response = socket.readpartial(128)
      expect(response).to start_with('HTTP/1.1 400 Bad Request')
      socket.close
    end

    expect(result[:exitstatus]).to eq(0), result[:output]
  end

  it 'upgrades HTTP/1.1 h2c and serves stream 1 plus a later stream' do
    result = h2c_server_result do |port|
      socket = connected_plain_socket(port)
      settings_payload = [4, 65_535].pack('nN')
      settings = Base64.urlsafe_encode64(settings_payload, padding: false)
      socket.write(
        "GET /upgrade HTTP/1.1\r\n" \
        "Host: localhost\r\n" \
        "Connection: Upgrade, HTTP2-Settings\r\n" \
        "Upgrade: h2c\r\n" \
        "HTTP2-Settings: #{settings}\r\n" \
        "\r\n"
      )

      response_head = +''
      response_head << socket.read(1) until response_head.include?("\r\n\r\n")
      expect(response_head).to start_with("HTTP/1.1 101 Switching Protocols\r\n")

      socket.write(h2_frame(4, 0, 0, ''))
      expect(h2_read_response_body(socket, 1)).to eq('GET:/upgrade:HTTP/2:http')
      socket.write(h2_frame(1, 0x5, 3, h2_request_header_block('/second')))
      expect(h2_read_response_body(socket, 3)).to eq('GET:/second:HTTP/2:http')
      socket.close
    end

    expect(result[:exitstatus]).to eq(0), result[:output]
  end

  it 'serves a large file-backed Rack response over h2c' do
    result = h2_large_response_result(tls: false)

    expect(result[:exitstatus]).to eq(0), result[:output]
    expect(result[:body].bytesize).to eq((300 * 1024) + 1024)
    expect(result[:body]).to start_with('a' * 1024)
    expect(result[:body]).to end_with('b' * 1024)
  end

  it 'resumes a deferred file-backed Rack response after WINDOW_UPDATE' do
    result = h2_large_response_result(tls: false, staged_window_update: true)

    expect(result[:exitstatus]).to eq(0), result[:output]
    expect(result[:body].bytesize).to eq((300 * 1024) + 1024)
    expect(result[:body]).to start_with('a' * 1024)
    expect(result[:body]).to end_with('b' * 1024)
  end

  it 'serves a large file-backed Rack response over TLS HTTP/2' do
    result = h2_large_response_result(tls: true)

    expect(result[:exitstatus]).to eq(0), result[:output]
    expect(result[:body].bytesize).to eq((300 * 1024) + 1024)
    expect(result[:body]).to start_with('a' * 1024)
    expect(result[:body]).to end_with('b' * 1024)
  end

  it 'serves an Extended CONNECT echo tunnel over h2c' do
    result = h2_tunnel_echo_result(tls: false)

    expect(result[:exitstatus]).to eq(0), result[:output]
  end

  it 'preserves the native HTTP/2 stream env object when active tracing context is required' do
    result = h2_tunnel_echo_result(tls: false, force_active_tracing: true)

    expect(result[:exitstatus]).to eq(0), result[:output]
  end

  it 'serves an Extended CONNECT echo tunnel over TLS HTTP/2' do
    result = h2_tunnel_echo_result(tls: true)

    expect(result[:exitstatus]).to eq(0), result[:output]
  end

  it 'serves a WebSocket-over-HTTP/2 raw frame echo over h2c' do
    result = h2_tunnel_echo_result(tls: false, websocket: true)

    expect(result[:exitstatus]).to eq(0), result[:output]
  end

  it 'serves a WebSocket-over-HTTP/2 raw frame echo over TLS HTTP/2' do
    result = h2_tunnel_echo_result(tls: true, websocket: true)

    expect(result[:exitstatus]).to eq(0), result[:output]
  end

  it 'keeps HTTP/2 stream usable after app-visible accept errors' do
    result = h2c_tunnel_server_result do |port|
      socket = connected_plain_socket(port)
      h2_start(socket)
      headers = h2_tunnel_echo_headers(path: '/double-accept', protocol: 'echo')
      socket.write(h2_frame(1, 0x5, 1, headers))
      expect(h2_read_response_body(socket, 1)).to eq('HTTP/2 stream was already accepted')
      socket.close
    end

    expect(result[:exitstatus]).to eq(0), result[:output]
  end

  it 'serializes a normal Rack response when Extended CONNECT is not accepted' do
    result = h2c_tunnel_server_result do |port|
      socket = connected_plain_socket(port)
      h2_start(socket)
      socket.write(h2_frame(1, 0x5, 1, h2_tunnel_echo_headers(path: '/normal')))
      expect(h2_read_response_body(socket, 1)).to eq('normal-connect-response')
      socket.close
    end

    expect(result[:exitstatus]).to eq(0), result[:output]
  end

  it 'rejects invalid Extended CONNECT requests before Rack execution' do
    invalid_extended_connect_header_blocks.each do |header_block|
      result = h2c_tunnel_server_result do |port|
        h2_expect_stream_reset(port, header_block)
      end
      expect(result[:exitstatus]).to eq(0), result[:output]
    end
  end

  it 'resets an accepted Extended CONNECT tunnel without a normal Rack response' do
    result = h2c_tunnel_server_result do |port|
      socket = connected_plain_socket(port)
      connect_headers = h2_headers(
        [':method', 'CONNECT'],
        [':scheme', 'http'],
        [':authority', 'localhost'],
        [':path', '/reset'],
        [':protocol', 'echo']
      )
      socket.write(
        "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n" \
        "#{h2_frame(4, 0, 0, '')}" \
        "#{h2_frame(1, 0x5, 1, connect_headers)}"
      )

      reset_frame = nil
      Timeout.timeout(5) do
        loop do
          frame = h2_read_frame(socket)
          h2_ack_settings(socket, frame)
          next unless frame[:type] == 3 && frame[:stream_id] == 1

          reset_frame = frame
          break
        end
      end
      expect(reset_frame[:payload].unpack1('N')).to eq(8)
      socket.close
    end

    expect(result[:exitstatus]).to eq(0), result[:output]
  end

  it 'resets an accepted Extended CONNECT tunnel over TLS HTTP/2' do
    result = h2c_tunnel_server_result(tls: true) do |port|
      socket = connected_tls_h2_socket(port)
      h2_start(socket)
      socket.write(h2_frame(1, 0x5, 1, h2_tunnel_echo_headers(path: '/reset')))

      reset_frame = h2_read_until(socket, type: 3, stream_id: 1)
      expect(reset_frame[:payload].unpack1('N')).to eq(8)
      socket.close
    end

    expect(result[:exitstatus]).to eq(0), result[:output]
  end

  it 'rejects every invalid h2c upgrade shape' do
    settings_payload = [4, 65_535].pack('nN')
    settings = Base64.urlsafe_encode64(settings_payload, padding: false)
    base = lambda do |version:, connection:, extra: ''|
      "GET /upgrade #{version}\r\n" \
        "Host: localhost\r\n" \
        "Connection: #{connection}\r\n" \
        "Upgrade: h2c\r\n" \
        "HTTP2-Settings: #{settings}\r\n" \
        "#{extra}\r\n"
    end

    expect_bad_h2c_upgrade(base.call(version: 'HTTP/1.1', connection: 'HTTP2-Settings'))
    expect_bad_h2c_upgrade(base.call(version: 'HTTP/1.1', connection: 'Upgrade'))
    expect_bad_h2c_upgrade(base.call(version: 'HTTP/1.1', connection: 'Upgrade, HTTP2-Settings', extra: "HTTP2-Settings: #{settings}\r\n"))
    expect_bad_h2c_upgrade(
      "GET /upgrade HTTP/1.1\r\nHost: localhost\r\nConnection: Upgrade, HTTP2-Settings\r\nUpgrade: h2c\r\nHTTP2-Settings: ***\r\n\r\n"
    )
    expect_bad_h2c_upgrade(base.call(version: 'HTTP/1.1', connection: 'Upgrade, HTTP2-Settings', extra: "Transfer-Encoding: chunked\r\n"))
    expect_bad_h2c_upgrade(base.call(version: 'HTTP/1.1', connection: 'Upgrade, HTTP2-Settings', extra: "Content-Length: 1\r\n"))
    expect_bad_h2c_upgrade(base.call(version: 'HTTP/1.0', connection: 'Upgrade, HTTP2-Settings'))
  end

  it 'serves higher-priority stream data first under constrained HTTP/2 flow control' do
    result = h2c_priority_server_result do |port|
      socket = connected_plain_socket(port)
      h2_start(socket, settings_payload: [4, 0].pack('nN'))
      h2_wait_for_settings_ack(socket)
      socket.write(
        "#{h2_priority_frame(1, 0, 1)}" \
        "#{h2_priority_frame(3, 0, 256)}" \
        "#{h2_frame(1, 0x5, 1, h2_request_header_block('/low'))}" \
        "#{h2_frame(1, 0x5, 3, h2_request_header_block('/high'))}"
      )

      h2_wait_for_response_headers(socket, [1, 3])
      h2_release_response_windows(socket, [1, 3])
      expect(h2_first_data_stream(socket)).to eq(3)
      socket.close
    end

    expect(result[:exitstatus]).to eq(0), result[:output]
  end

  it 'applies reprioritization after request headers have started' do
    result = h2c_priority_server_result do |port|
      socket = connected_plain_socket(port)
      h2_start(socket, settings_payload: [4, 0].pack('nN'))
      h2_wait_for_settings_ack(socket)
      socket.write(
        "#{h2_priority_frame(1, 0, 1)}" \
        "#{h2_priority_frame(3, 0, 128)}" \
        "#{h2_frame(1, 0x4, 1, h2_request_header_block('/low'))}" \
        "#{h2_frame(1, 0x4, 3, h2_request_header_block('/high'))}" \
        "#{h2_priority_frame(1, 0, 256)}" \
        "#{h2_frame(0, 0x1, 1, '')}" \
        "#{h2_frame(0, 0x1, 3, '')}"
      )

      h2_wait_for_response_headers(socket, [1, 3])
      h2_release_response_windows(socket, [1, 3])
      expect(h2_first_data_stream(socket)).to eq(1)
      socket.close
    end

    expect(result[:exitstatus]).to eq(0), result[:output]
  end

  it 'schedules by dependency path before descendant weight' do
    result = h2c_priority_server_result(expected_paths: ['/high', '/mid', '/low']) do |port|
      socket = connected_plain_socket(port)
      h2_start(socket, settings_payload: [4, 0].pack('nN'))
      h2_wait_for_settings_ack(socket)
      socket.write(
        "#{h2_priority_frame(1, 0, 1)}" \
        "#{h2_priority_frame(3, 1, 256)}" \
        "#{h2_priority_frame(5, 0, 128)}" \
        "#{h2_frame(1, 0x5, 1, h2_request_header_block('/low'))}" \
        "#{h2_frame(1, 0x5, 3, h2_request_header_block('/high'))}" \
        "#{h2_frame(1, 0x5, 5, h2_request_header_block('/mid'))}"
      )

      h2_wait_for_response_headers(socket, [1, 3, 5])
      h2_release_response_windows(socket, [1, 3, 5])
      expect(h2_first_data_stream(socket)).to eq(5)
      socket.close
    end

    expect(result[:exitstatus]).to eq(0), result[:output]
  end

  it 'applies exclusive priority rewrites to existing root children' do
    result = h2c_priority_server_result(expected_paths: ['/high', '/mid', '/low']) do |port|
      socket = connected_plain_socket(port)
      h2_start(socket, settings_payload: [4, 0].pack('nN'))
      h2_wait_for_settings_ack(socket)
      socket.write(
        "#{h2_priority_frame(1, 0, 1)}" \
        "#{h2_priority_frame(3, 0, 256)}" \
        "#{h2_priority_frame(5, 0, 128)}" \
        "#{h2_priority_frame(1, 0, 256, exclusive: true)}" \
        "#{h2_frame(1, 0x5, 1, h2_request_header_block('/low'))}" \
        "#{h2_frame(1, 0x5, 3, h2_request_header_block('/high'))}" \
        "#{h2_frame(1, 0x5, 5, h2_request_header_block('/mid'))}"
      )

      h2_wait_for_response_headers(socket, [1, 3, 5])
      h2_release_response_windows(socket, [1, 3, 5])
      expect(h2_first_data_stream(socket)).to eq(1)
      socket.close
    end

    expect(result[:exitstatus]).to eq(0), result[:output]
  end

  it 'resets invalid priority self-dependency' do
    result = h2c_priority_server_result do |port|
      socket = connected_plain_socket(port)
      h2_start(socket)
      socket.write(h2_priority_frame(1, 1, 16))

      reset_frame = h2_read_until(socket, type: 3, stream_id: 1)
      expect(h2_rst_stream_error_code(reset_frame)).to eq(1)
      socket.close
    end

    expect(result[:exitstatus]).to eq(0), result[:output]
  end
end
