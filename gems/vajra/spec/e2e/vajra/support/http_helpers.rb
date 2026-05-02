# frozen_string_literal: true

# Copyright Codevedas Inc. 2025-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

module VajraE2EHttpHelpers
  def parse_http_response(response)
    headers, body = response.split("\r\n\r\n", 2)
    header_lines = headers.split("\r\n")
    status_line = header_lines.shift
    parsed_headers = header_lines.to_h do |line|
      name, value = line.split(':', 2)
      [name, value.strip]
    end

    { raw: response, status_line:, headers: parsed_headers, body: body || '' }
  end

  def read_http_response(socket, buffered_bytes: +'')
    Timeout.timeout(2) do
      response = String.new(buffered_bytes)

      response << socket.readpartial(4096) until response.include?("\r\n\r\n")

      headers, body = response.split("\r\n\r\n", 2)
      content_length_header = headers.lines.find { |line| line.start_with?('Content-Length: ') }
      content_length = content_length_header ? Integer(content_length_header.delete_prefix('Content-Length: ').strip) : 0

      body << socket.readpartial(4096) while body.bytesize < content_length

      complete_response = "#{headers}\r\n\r\n#{body.byteslice(0, content_length)}"
      trailing_bytes = body.byteslice(content_length..) || +''

      [parse_http_response(complete_response), trailing_bytes]
    end
  end

  def request_response(port: disposable_listener_port)
    Open3.popen2e(vajra_env(port:), *vajra_command, chdir: VajraE2EHelpers::PACKAGE_ROOT) do |_stdin, output, wait_thread|
      selected_port = wait_for_banner(output)

      socket = TCPSocket.new(VajraE2EHelpers::LISTENER_HOST, selected_port)
      socket.write("GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
      response = socket.read
      socket.close

      status = stop_process(wait_thread)

      { exitstatus: status.exitstatus, response: response, port: selected_port }
    ensure
      cleanup_process(wait_thread, output)
    end
  end

  def request_response_from_inline_start(env:, timeout: 15)
    Open3.popen2e(vajra_env.merge(env), *inline_start_command, chdir: VajraE2EHelpers::PACKAGE_ROOT) do |_stdin, output, wait_thread|
      selected_port = wait_for_banner(output)

      socket = TCPSocket.new(VajraE2EHelpers::LISTENER_HOST, selected_port)
      socket.write("GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
      response = socket.read
      socket.close

      status = stop_process(wait_thread, timeout:)

      { exitstatus: status.exitstatus, response: response, port: selected_port, output: output.read }
    ensure
      cleanup_process(wait_thread, output)
    end
  end

  def sequential_request_result(port: disposable_listener_port)
    Open3.popen2e(vajra_env(port:), *vajra_command, chdir: VajraE2EHelpers::PACKAGE_ROOT) do |_stdin, output, wait_thread|
      selected_port = wait_for_banner(output)

      socket = TCPSocket.new(VajraE2EHelpers::LISTENER_HOST, selected_port)
      buffered_bytes = +''
      socket.write("GET /first HTTP/1.1\r\nHost: localhost\r\n\r\n")
      first_response, buffered_bytes = read_http_response(socket, buffered_bytes:)

      socket.write("GET /second HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
      second_response, buffered_bytes = read_http_response(socket, buffered_bytes:)
      connection_closed = Timeout.timeout(2) { socket.read == '' }
      socket.close

      status = stop_process(wait_thread)

      {
        exitstatus: status.exitstatus,
        first_response:,
        second_response:,
        connection_closed:,
        trailing_bytes: buffered_bytes,
        port: selected_port
      }
    ensure
      cleanup_process(wait_thread, output)
    end
  end

  def pipelined_request_result(port: disposable_listener_port)
    Open3.popen2e(vajra_env(port:), *vajra_command, chdir: VajraE2EHelpers::PACKAGE_ROOT) do |_stdin, output, wait_thread|
      selected_port = wait_for_banner(output)

      socket = TCPSocket.new(VajraE2EHelpers::LISTENER_HOST, selected_port)
      buffered_bytes = +''
      socket.write(
        "GET /first HTTP/1.1\r\n" \
        "Host: localhost\r\n\r\n" \
        "GET /second HTTP/1.1\r\n" \
        "Host: localhost\r\n" \
        "Connection: close\r\n\r\n"
      )
      first_response, buffered_bytes = read_http_response(socket, buffered_bytes:)
      second_response, buffered_bytes = read_http_response(socket, buffered_bytes:)
      connection_closed = Timeout.timeout(2) { socket.read == '' }
      socket.close

      status = stop_process(wait_thread)

      {
        exitstatus: status.exitstatus,
        first_response:,
        second_response:,
        connection_closed:,
        trailing_bytes: buffered_bytes,
        port: selected_port
      }
    ensure
      cleanup_process(wait_thread, output)
    end
  end
end
