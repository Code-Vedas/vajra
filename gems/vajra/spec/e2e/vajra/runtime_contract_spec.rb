# frozen_string_literal: true

# Copyright Codevedas Inc. 2025-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

require_relative 'support'

RSpec.describe 'Vajra runtime contract', :e2e, :integration do # rubocop:disable RSpec/DescribeClass
  it 'serves the baseline close-delimited success response' do
    result = request_response
    response = parse_http_response(result[:response])

    expect(result[:exitstatus]).to eq(0)
    expect(response[:status_line]).to eq('HTTP/1.1 200 OK')
    expect(response[:headers]).to include(
      'Content-Type' => 'text/plain',
      'Content-Length' => '2',
      'Connection' => 'close'
    )
    expect(response[:body]).to eq('OK')
  end

  it 'reuses an HTTP/1.1 connection for a sequential request and closes when requested' do
    result = sequential_request_result

    expect(result[:exitstatus]).to eq(0)
    expect(result[:first_response]).to include(
      status_line: 'HTTP/1.1 200 OK',
      body: 'OK'
    )
    expect(result[:first_response][:headers]).to include(
      'Content-Type' => 'text/plain',
      'Content-Length' => '2'
    )
    expect(result[:first_response][:headers]).not_to include('Connection')
    expect(result[:second_response]).to include(
      status_line: 'HTTP/1.1 200 OK',
      body: 'OK'
    )
    expect(result[:second_response][:headers]).to include(
      'Content-Type' => 'text/plain',
      'Content-Length' => '2',
      'Connection' => 'close'
    )
    expect(result[:connection_closed]).to be(true)
    expect(result[:trailing_bytes]).to eq('')
  end

  it 'preserves the pipelined next request across the keep-alive loop' do
    result = pipelined_request_result

    expect(result[:exitstatus]).to eq(0)
    expect(result[:first_response]).to include(
      status_line: 'HTTP/1.1 200 OK',
      body: 'OK'
    )
    expect(result[:first_response][:headers]).not_to include('Connection')
    expect(result[:second_response]).to include(
      status_line: 'HTTP/1.1 200 OK',
      body: 'OK'
    )
    expect(result[:second_response][:headers]).to include('Connection' => 'close')
    expect(result[:connection_closed]).to be(true)
    expect(result[:trailing_bytes]).to eq('')
  end

  it 'closes an idle reusable connection when the next request head times out' do
    result = idle_keep_alive_timeout_result

    expect(result[:exitstatus]).to eq(0)
    expect(result[:response]).to include(
      status_line: 'HTTP/1.1 200 OK',
      body: 'OK'
    )
    expect(result[:response][:headers]).to include(
      'Content-Type' => 'text/plain',
      'Content-Length' => '2'
    )
    expect(result[:response][:headers]).not_to include('Connection')
    expect(result[:connection_closed]).to be(true)
  end

  it 'closes an incomplete request without producing a success response' do
    result = raw_request_result(
      request: "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n"
    )

    expect(result[:exitstatus]).to eq(0)
    expect(result[:response]).to eq('')
    expect(result[:output]).not_to include('request rejected (400 bad request)')
    expect(result[:output]).not_to include('HTTP/1.1 200 OK')
  end

  it 'accepts a fragmented request when the header boundary arrives across multiple writes' do
    result = fragmented_request_result(
      chunks: [
        "GET /fragmented HTTP/1.1\r\nHost: localhost\r\nConnection: close\r",
        "\n",
        "\r",
        "\n"
      ]
    )
    response = parse_http_response(result[:response])

    expect(result[:exitstatus]).to eq(0)
    expect(response).to include(
      status_line: 'HTTP/1.1 200 OK',
      body: 'OK'
    )
    expect(response[:headers]).to include(
      'Content-Type' => 'text/plain',
      'Content-Length' => '2'
    )
  end
end
