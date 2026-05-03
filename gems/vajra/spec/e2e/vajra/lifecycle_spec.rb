# frozen_string_literal: true

# Copyright Codevedas Inc. 2025-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

require_relative 'support'

RSpec.describe 'Vajra lifecycle', :e2e, :integration do # rubocop:disable RSpec/DescribeClass
  it 'shuts down cleanly while idle and releases the listener for immediate restart' do
    shutdown = idle_shutdown

    expect(shutdown[:exitstatus]).to eq(0)
    expect(shutdown[:output]).not_to include('accept failed')

    rebound_server = bind_port(port: shutdown[:port])
    rebound_server.close

    expect(request_response(port: shutdown[:port])).to include(
      exitstatus: 0,
      response: a_string_including('HTTP/1.1 200 OK')
    )
  end

  it 'supports programmatic Vajra.stop and releases the listener' do
    shutdown = programmatic_shutdown

    expect(shutdown[:exitstatus]).to eq(0), shutdown[:output]
    expect(shutdown[:output]).not_to include('accept failed')
  end

  it 'survives repeated process start stop thrashing and releases the listener every time' do
    current_port = disposable_listener_port

    thrash_cycles.times do
      request = request_response(port: current_port)

      expect(request).to include(
        exitstatus: 0,
        response: a_string_including('HTTP/1.1 200 OK')
      )
      current_port = request[:port]

      shutdown = idle_shutdown(port: current_port)
      expect(shutdown[:exitstatus]).to eq(0)
      expect(shutdown[:output]).not_to include('accept failed')

      rebound_server = bind_port(port: current_port)
      rebound_server.close
    end
  end

  it 'accepts a request when only bytes after the header boundary exceed the configured limit' do
    body = 'b' * 4096
    result = request_with_body_result(
      env: {
        'VAJRA_PORT' => disposable_listener_port.to_s,
        'VAJRA_MAX_REQUEST_HEAD_BYTES' => '128'
      },
      body:
    )

    expect(result[:exitstatus]).to eq(0)
    expect(result[:response]).to include('HTTP/1.1 200 OK')
    expect(result[:output]).not_to include('request head exceeds maximum size')
  end

  it 'closes after responding to a request with unread body framing' do
    result = request_with_body_result(
      env: { 'VAJRA_PORT' => disposable_listener_port.to_s },
      body: 'body'
    )
    response = parse_http_response(result[:response])

    expect(result[:exitstatus]).to eq(0)
    expect(response).to include(
      status_line: 'HTTP/1.1 200 OK',
      body: 'OK'
    )
    expect(response[:headers]).to include(
      'content-type' => 'text/plain',
      'content-length' => '2',
      'connection' => 'close'
    )
  end
end
