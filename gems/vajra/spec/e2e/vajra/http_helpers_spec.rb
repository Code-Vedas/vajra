# frozen_string_literal: true

# Copyright Codevedas Inc. 2025-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

require_relative 'support'

RSpec.describe 'Vajra e2e HTTP helpers', :e2e, :integration do # rubocop:disable RSpec/DescribeClass
  let(:helper_host) do
    Class.new do
      include VajraE2ESupport
    end.new
  end

  it 'raises a diagnosable error for malformed response header lines' do
    raw_response = "HTTP/1.1 200 OK\r\nBroken-Header\r\n\r\nOK"

    expect { helper_host.parse_http_response(raw_response) }.to raise_error(
      ArgumentError,
      /invalid HTTP response header line: "Broken-Header"/
    )
  end

  it 'parses Content-Length case-insensitively when reading a response' do
    socket = instance_double(TCPSocket)

    allow(socket).to receive(:readpartial).and_return(
      "HTTP/1.1 200 OK\r\ncontent-length: 2\r\n\r\nOK"
    )

    response, trailing_bytes = helper_host.read_http_response(socket)

    expect(response).to include(
      status_line: 'HTTP/1.1 200 OK',
      body: 'OK'
    )
    expect(response[:headers]).to include('content-length' => '2')
    expect(trailing_bytes).to eq('')
  end
end
