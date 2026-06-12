# frozen_string_literal: true

# Copyright Codevedas Inc. 2025-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

require_relative 'support'
require 'openssl'
require 'tmpdir'

# rubocop:disable RSpec/DescribeMethod, RSpec/SpecFilePathFormat
RSpec.describe Vajra, 'Rack hijack integration', :e2e, :integration do
  def rack_hijack_raw_response(script:, request:)
    managed_popen2e(
      vajra_env(port: disposable_listener_port),
      *inline_ruby_command(script),
      chdir: VajraE2EHelpers::PACKAGE_ROOT
    ) do |_stdin, output, wait_thread|
      startup_output = []
      selected_port = wait_for_banner(output, captured_lines: startup_output)

      socket = TCPSocket.new(VajraE2EHelpers::LISTENER_HOST, selected_port)
      socket.write(request)
      response = Timeout.timeout(VajraE2EHelpers::HTTP_RESPONSE_READ_TIMEOUT_SECONDS) { socket.read }
      socket.close

      status = stop_process(wait_thread)
      { exitstatus: status.exitstatus, response:, output: "#{startup_output.join}#{output.read}", port: selected_port }
    ensure
      socket&.close unless socket&.closed?
      cleanup_process(wait_thread, output)
    end
  end

  def write_hijack_test_certificate(root)
    key = OpenSSL::PKey::RSA.new(2048)
    cert = OpenSSL::X509::Certificate.new
    cert.version = 2
    cert.serial = 1
    cert.subject = OpenSSL::X509::Name.parse('/CN=localhost')
    cert.issuer = cert.subject
    cert.public_key = key.public_key
    cert.not_before = Time.now.utc - 60
    cert.not_after = Time.now.utc + 3600
    add_hijack_certificate_extensions(cert)
    cert.sign(key, OpenSSL::Digest.new('SHA256'))

    cert_path = File.join(root, 'server.crt')
    key_path = File.join(root, 'server.key')
    File.write(cert_path, cert.to_pem)
    File.write(key_path, key.to_pem)
    [cert_path, key_path]
  end

  def add_hijack_certificate_extensions(cert)
    extension_factory = OpenSSL::X509::ExtensionFactory.new
    extension_factory.subject_certificate = cert
    extension_factory.issuer_certificate = cert
    cert.add_extension(extension_factory.create_extension('subjectAltName', 'DNS:localhost,IP:127.0.0.1'))
  end

  def connected_hijack_tls_socket(port, alpn_protocols:)
    tcp_socket = TCPSocket.new(VajraE2EHelpers::LISTENER_HOST, port)
    context = OpenSSL::SSL::SSLContext.new
    context.verify_mode = OpenSSL::SSL::VERIFY_NONE
    context.alpn_protocols = alpn_protocols
    ssl_socket = OpenSSL::SSL::SSLSocket.new(tcp_socket, context)
    ssl_socket.hostname = 'localhost'
    ssl_socket.connect
    ssl_socket
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
    payload = length.zero? ? ''.b : socket.read(length)
    { length:, type:, flags:, stream_id: stream_id & 0x7fff_ffff, payload: }
  end

  def h2_request_header_block
    +"\x82".b + "\x84".b + "\x87".b + "\x41".b + h2_string('localhost')
  end

  def h2_hijack_absence_body(socket)
    socket.write(
      "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n" \
      "#{h2_frame(4, 0, 0, '')}" \
      "#{h2_frame(1, 0x5, 1, h2_request_header_block)}"
    )

    body = +''.b
    Timeout.timeout(5) do
      loop do
        frame = h2_read_frame(socket)
        socket.write(h2_frame(4, 0x1, 0, '')) if frame[:type] == 4 && frame[:flags].nobits?(0x1)
        body << frame[:payload] if frame[:type].zero?
        break if frame[:type].zero? && frame[:flags].anybits?(0x1)
      end
    end
    body
  end

  it 'lets an HTTP/1.1 Rack app take over the raw connection' do
    script = <<~RUBY
      require "vajra"

      Vajra::Internal::RackExecution.install!(
        lambda do |env|
          io = env.fetch("rack.hijack").call
          io.write "HTTP/1.1 101 Switching Protocols\\r\\n"
          io.write "Upgrade: websocket\\r\\n"
          io.write "Connection: Upgrade\\r\\n"
          io.write "\\r\\n"
          io.write "raw-hijack"
          io.close
          [500, { "Content-Type" => "text/plain" }, ["ignored"]]
        end
      )

      Vajra.start
    RUBY

    result = rack_hijack_raw_response(
      script:,
      request:
        "GET /socket HTTP/1.1\r\n" \
        "Host: localhost\r\n" \
        "Connection: Upgrade\r\n" \
        "Upgrade: websocket\r\n\r\n"
    )

    expect(result[:exitstatus]).to eq(0)
    expect(result[:response]).to eq(
      "HTTP/1.1 101 Switching Protocols\r\n" \
      "Upgrade: websocket\r\n" \
      "Connection: Upgrade\r\n\r\n" \
      'raw-hijack'
    )
  end

  it 'preserves rack.hijack when active tracing context is required' do
    script = <<~RUBY
      require "vajra"

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
      Vajra::Internal::RackExecution.install!(
        lambda do |env|
          io = env.fetch("rack.hijack").call
          io.write "HTTP/1.1 101 Switching Protocols\\r\\n"
          io.write "Connection: Upgrade\\r\\n"
          io.write "\\r\\n"
          io.write "traced-hijack"
          io.close
          [500, { "Content-Type" => "text/plain" }, ["ignored"]]
        end
      )

      Vajra.start
    RUBY

    result = rack_hijack_raw_response(
      script:,
      request:
        "GET /socket HTTP/1.1\r\n" \
        "Host: localhost\r\n" \
        "Connection: Upgrade\r\n" \
        "Upgrade: websocket\r\n\r\n"
    )

    expect(result[:exitstatus]).to eq(0)
    expect(result[:response]).to eq(
      "HTTP/1.1 101 Switching Protocols\r\n" \
      "Connection: Upgrade\r\n\r\n" \
      'traced-hijack'
    )
  end

  it 'makes the full hijack callable single-use' do
    script = <<~RUBY
      require "vajra"

      Vajra::Internal::RackExecution.install!(
        lambda do |env|
          io = env.fetch("rack.hijack").call
          error_message = begin
            env.fetch("rack.hijack").call
            "no-error"
          rescue => e
            "\#{e.class}:\#{e.message}"
          end
          body = error_message.b
          io.write "HTTP/1.1 200 OK\\r\\nContent-Length: \#{body.bytesize}\\r\\nConnection: close\\r\\n\\r\\n"
          io.write body
          io.close
          [200, {}, []]
        end
      )

      Vajra.start
    RUBY

    result = rack_hijack_raw_response(
      script:,
      request: "GET /once HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"
    )

    expect(result[:response]).to include('IOError:rack.hijack was already called')
  end

  it 'lets an HTTP/1.0 Rack app take over the raw connection' do
    script = <<~RUBY
      require "vajra"

      Vajra::Internal::RackExecution.install!(
        lambda do |env|
          io = env.fetch("rack.hijack").call
          io.write "HTTP/1.0 200 OK\\r\\nContent-Length: 8\\r\\n\\r\\nhijack10"
          io.close
          [500, {}, ["ignored"]]
        end
      )

      Vajra.start
    RUBY

    result = rack_hijack_raw_response(
      script:,
      request: "GET /legacy HTTP/1.0\r\nHost: localhost\r\n\r\n"
    )

    expect(result[:response]).to eq("HTTP/1.0 200 OK\r\nContent-Length: 8\r\n\r\nhijack10")
  end

  it 'raises when the app calls full hijack after normal response commit' do
    script = <<~RUBY
      require "vajra"

      saved_hijack = nil
      Vajra::Internal::RackExecution.install!(
        lambda do |env|
          if env["PATH_INFO"] == "/commit"
            saved_hijack = env.fetch("rack.hijack")
            [200, { "Content-Type" => "text/plain" }, ["committed"]]
          else
            message = begin
              saved_hijack.call
              "no-error"
            rescue => e
              "\#{e.class}:\#{e.message}"
            end
            [200, { "Content-Type" => "text/plain" }, [message]]
          end
        end
      )

      Vajra.start
    RUBY

    result = rack_hijack_raw_response(
      script:,
      request:
        "GET /commit HTTP/1.1\r\nHost: localhost\r\n\r\n" \
        "GET /after HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"
    )

    expect(result[:response]).to include('committed')
    expect(result[:response]).to include('IOError:rack.hijack is no longer available')
  end

  it 'does not reuse a hijacked keep-alive socket for pipelined requests' do
    script = <<~RUBY
      require "vajra"

      Vajra::Internal::RackExecution.install!(
        lambda do |env|
          io = env.fetch("rack.hijack").call
          io.write "HTTP/1.1 200 OK\\r\\nContent-Length: 7\\r\\nConnection: close\\r\\n\\r\\nhijack!"
          io.close
          [200, {}, ["ignored"]]
        end
      )

      Vajra.start
    RUBY

    result = rack_hijack_raw_response(
      script:,
      request:
        "GET /first HTTP/1.1\r\nHost: localhost\r\n\r\n" \
        "GET /second HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"
    )

    expect(result[:response]).to eq("HTTP/1.1 200 OK\r\nContent-Length: 7\r\nConnection: close\r\n\r\nhijack!")
  end

  it 'allows hijack after the app consumes the request body' do
    script = <<~RUBY
      require "vajra"

      Vajra::Internal::RackExecution.install!(
        lambda do |env|
          body = env.fetch("rack.input").read
          io = env.fetch("rack.hijack").call
          payload = "body=\#{body}".b
          io.write "HTTP/1.1 200 OK\\r\\nContent-Length: \#{payload.bytesize}\\r\\nConnection: close\\r\\n\\r\\n"
          io.write payload
          io.close
          [200, {}, []]
        end
      )

      Vajra.start
    RUBY

    result = rack_hijack_raw_response(
      script:,
      request:
        "POST /body HTTP/1.1\r\n" \
        "Host: localhost\r\n" \
        "Content-Length: 6\r\n" \
        "Connection: close\r\n\r\n" \
        'abcdef'
    )

    expect(result[:response]).to include("\r\n\r\nbody=abcdef")
  end

  it 'allows hijack after the app consumes an exact fixed-length request body read' do
    script = <<~RUBY
      require "vajra"

      Vajra::Internal::RackExecution.install!(
        lambda do |env|
          body = env.fetch("rack.input").read(6)
          io = env.fetch("rack.hijack").call
          payload = "body=\#{body}".b
          io.write "HTTP/1.1 200 OK\\r\\nContent-Length: \#{payload.bytesize}\\r\\nConnection: close\\r\\n\\r\\n"
          io.write payload
          io.close
          [200, {}, []]
        end
      )

      Vajra.start
    RUBY

    result = rack_hijack_raw_response(
      script:,
      request:
        "POST /body HTTP/1.1\r\n" \
        "Host: localhost\r\n" \
        "Content-Length: 6\r\n" \
        "Connection: close\r\n\r\n" \
        'abcdef'
    )

    expect(result[:response]).to include("\r\n\r\nbody=abcdef")
  end

  it 'lets a TLS HTTP/1.1 Rack app take over the decrypted connection' do
    Dir.mktmpdir do |root|
      cert_path, key_path = write_hijack_test_certificate(root)
      script = <<~RUBY
        require "vajra"

        Vajra::Internal::RackExecution.install!(
          lambda do |env|
            io = env.fetch("rack.hijack").call
            io.write "HTTP/1.1 200 OK\\r\\n"
            io.write "Content-Length: 10\\r\\n"
            io.write "Connection: close\\r\\n"
            io.write "\\r\\n"
            io.write "tls-hijack"
            io.close
            [500, { "Content-Type" => "text/plain" }, ["ignored"]]
          end
        )

        Vajra.start(
          tls: true,
          http2: false,
          tls_certificate: ENV.fetch("TLS_CERTIFICATE"),
          tls_private_key: ENV.fetch("TLS_PRIVATE_KEY")
        )
      RUBY

      managed_popen2e(
        vajra_env(port: disposable_listener_port).merge('TLS_CERTIFICATE' => cert_path, 'TLS_PRIVATE_KEY' => key_path),
        *inline_ruby_command(script),
        chdir: VajraE2EHelpers::PACKAGE_ROOT
      ) do |_stdin, output, wait_thread|
        startup_output = []
        selected_port = wait_for_banner(output, captured_lines: startup_output)
        socket = connected_hijack_tls_socket(selected_port, alpn_protocols: ['http/1.1'])
        socket.write("GET /tls-hijack HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
        response = Timeout.timeout(VajraE2EHelpers::HTTP_RESPONSE_READ_TIMEOUT_SECONDS) { socket.read }
        socket.close
        status = stop_process(wait_thread)

        expect(status.exitstatus).to eq(0), "#{startup_output.join}#{output.read}"
        expect(response).to eq(
          "HTTP/1.1 200 OK\r\n" \
          "Content-Length: 10\r\n" \
          "Connection: close\r\n\r\n" \
          'tls-hijack'
        )
      ensure
        socket&.close unless socket&.closed?
        cleanup_process(wait_thread, output)
      end
    end
  end

  it 'rejects partial Rack hijack response headers' do
    script = <<~RUBY
      require "vajra"

      Vajra::Internal::RackExecution.install!(
        lambda do |_env|
          [200, { "rack.hijack" => proc {} }, []]
        end
      )

      Vajra.start
    RUBY

    result = rack_app_request_result(
      script:,
      request: "GET /partial HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"
    )
    response = parse_http_response(result[:response])

    expect(response[:status_line]).to eq('HTTP/1.1 500 Internal Server Error')
    expect(result[:output]).to include('partial Rack hijack is not supported')
  end

  it 'does not expose full hijack to HTTP/2 requests' do
    Dir.mktmpdir do |root|
      cert_path, key_path = write_hijack_test_certificate(root)
      script = <<~RUBY
        require "vajra"

        Vajra::Internal::RackExecution.install!(
          lambda do |env|
            body = env.key?("rack.hijack") ? "hijack-present" : "hijack-absent"
            [200, { "Content-Type" => "text/plain" }, [body]]
          end
        )

        Vajra.start(
          tls: true,
          http2: true,
          tls_certificate: ENV.fetch("TLS_CERTIFICATE"),
          tls_private_key: ENV.fetch("TLS_PRIVATE_KEY")
        )
      RUBY

      managed_popen2e(
        vajra_env(port: disposable_listener_port).merge('TLS_CERTIFICATE' => cert_path, 'TLS_PRIVATE_KEY' => key_path),
        *inline_ruby_command(script),
        chdir: VajraE2EHelpers::PACKAGE_ROOT
      ) do |_stdin, output, wait_thread|
        startup_output = []
        selected_port = wait_for_banner(output, captured_lines: startup_output)
        socket = connected_hijack_tls_socket(selected_port, alpn_protocols: ['h2'])
        expect(socket.alpn_protocol).to eq('h2')
        body = h2_hijack_absence_body(socket)
        socket.close
        status = stop_process(wait_thread)

        expect(status.exitstatus).to eq(0), "#{startup_output.join}#{output.read}"
        expect(body).to eq('hijack-absent')
      ensure
        socket&.close unless socket&.closed?
        cleanup_process(wait_thread, output)
      end
    end
  end
end
# rubocop:enable RSpec/DescribeMethod, RSpec/SpecFilePathFormat
