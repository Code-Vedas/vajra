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

      Process.kill('KILL', wait_thread.pid)
      wait_thread.value

      response
    end
  end

  def startup_failure
    Open3.popen2e(*vajra_command, chdir: VajraE2EHelpers::PACKAGE_ROOT) do |_stdin, output, wait_thread|
      status = Timeout.timeout(15) { wait_thread.value }
      { exitstatus: status.exitstatus, output: output.read }
    end
  end

  def bind_port
    TCPServer.new('0.0.0.0', 3000)
  end

  it 'boots and serves a basic HTTP response' do
    expect(request_response).to include('HTTP/1.1 200 OK')
  end

  it 'fails startup with actionable bind diagnostics and releases startup resources' do
    blocking_server = bind_port

    begin
      failure = startup_failure

      expect(failure).to match(
        exitstatus: a_value > 0,
        output: a_string_including('Unable to start Vajra: listener bind failed for port 3000')
      )
      expect(failure[:output]).not_to include('Vajra listening on port 3000')
    ensure
      blocking_server.close
    end

    rebound_server = bind_port
    rebound_server.close
  end
end
