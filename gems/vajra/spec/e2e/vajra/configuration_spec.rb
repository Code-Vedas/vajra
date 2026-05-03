# frozen_string_literal: true

# Copyright Codevedas Inc. 2025-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

require_relative 'support'

RSpec.describe 'Vajra configuration', :e2e, :integration do # rubocop:disable RSpec/DescribeClass
  it 'fails startup with actionable bind diagnostics and releases startup resources' do
    blocking_server = bind_port
    blocked_port = blocking_server.addr[1]

    begin
      failure = startup_failure(port: blocked_port)

      expect(failure).to match(
        exitstatus: be_positive,
        output: a_string_including(
          "Unable to start Vajra: listener bind failed for port #{blocked_port}"
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
      output: a_string_including('Unable to start Vajra: unknown start option: potr')
    )
  end

  it 'lets Ruby configure the listener port when VAJRA_PORT is unset' do
    request = request_response_from_inline_start(env: { 'RUBY_PORT' => disposable_listener_port.to_s })

    expect(request[:exitstatus]).to eq(0)
    expect(request[:port]).to be_positive
    expect(request[:response]).to include('HTTP/1.1 200 OK')
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
end
