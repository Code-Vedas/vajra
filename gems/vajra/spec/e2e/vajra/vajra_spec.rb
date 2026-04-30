# frozen_string_literal: true

# Copyright Codevedas Inc. 2025-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

require_relative '../spec_helper'

RSpec.describe Vajra, :e2e, :integration do
  def request_response
    Open3.popen2e(*vajra_command, chdir: VajraE2EHelpers::PACKAGE_ROOT) do |_stdin, output, wait_thread|
      wait_for_banner(output)

      socket = TCPSocket.new('127.0.0.1', 3000)
      socket.write("GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
      response = socket.read
      socket.close

      Process.kill('INT', wait_thread.pid)
      status = wait_thread.value

      { exitstatus: status.exitstatus, response: response }
    end
  end

  it 'boots and serves a basic HTTP response' do
    expect(request_response).to match(
      exitstatus: 0,
      response: a_string_including('HTTP/1.1 200 OK')
    )
  end
end
