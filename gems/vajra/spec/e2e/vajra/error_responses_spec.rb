# frozen_string_literal: true

# Copyright Codevedas Inc. 2025-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

require_relative 'support'

RSpec.describe 'Vajra error responses', :e2e, :integration do # rubocop:disable RSpec/DescribeClass
  it 'rejects a malformed request line with 400 Bad Request' do
    result = raw_request_result(
      request: "GET /only-two-parts\r\nHost: localhost\r\nConnection: close\r\n\r\n"
    )
    response = parse_http_response(result[:response])

    expect(result[:exitstatus]).to eq(0)
    expect(response).to include(
      status_line: 'HTTP/1.1 400 Bad Request',
      body: 'Bad Request'
    )
    expect(response[:headers]).to include(
      'content-type' => 'text/plain',
      'content-length' => '11',
      'connection' => 'close'
    )
    expect(result[:output]).to include('request rejected (400 bad request)')
    expect(result[:output]).not_to include('HTTP/1.1 200 OK')
  end

  it 'rejects an invalid HTTP version with 400 Bad Request' do
    result = raw_request_result(
      request: "GET / HTTP/2.0\r\nHost: localhost\r\nConnection: close\r\n\r\n"
    )
    response = parse_http_response(result[:response])

    expect(result[:exitstatus]).to eq(0)
    expect(response).to include(
      status_line: 'HTTP/1.1 400 Bad Request',
      body: 'Bad Request'
    )
    expect(response[:headers]).to include(
      'content-type' => 'text/plain',
      'content-length' => '11',
      'connection' => 'close'
    )
  end

  it 'rejects an oversized header section with 431 Request Header Fields Too Large' do
    result = raw_request_result(
      port: disposable_listener_port,
      env: { 'VAJRA_MAX_REQUEST_HEAD_BYTES' => '128' },
      request: "GET / HTTP/1.1\r\nHost: localhost\r\nX-Oversized: #{'a' * 256}\r\nConnection: close\r\n\r\n"
    )
    response = parse_http_response(result[:response])

    expect(result[:exitstatus]).to eq(0)
    expect(response).to include(
      status_line: 'HTTP/1.1 431 Request Header Fields Too Large',
      body: 'Request Header Fields Too Large'
    )
    expect(response[:headers]).to include(
      'content-type' => 'text/plain',
      'content-length' => '31',
      'connection' => 'close'
    )
    expect(result[:output]).to include('request rejected (431 request header fields too large)')
  end
end
