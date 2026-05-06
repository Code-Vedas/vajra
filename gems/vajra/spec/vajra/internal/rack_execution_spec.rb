# frozen_string_literal: true

# Copyright Codevedas Inc. 2025-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

RSpec.describe Vajra::Internal::RackExecution do
  after do
    described_class.uninstall!
  end

  it 'tracks whether a Rack app is installed' do
    expect(described_class.installed?).to be(false)

    described_class.install!(->(_env) { [200, { 'Content-Type' => 'text/plain' }, ['OK']] })

    expect(described_class.installed?).to be(true)
  end

  it 'builds the Rack env and normalizes the response body and headers' do
    captured_env = nil
    headers = Class.new do
      def each
        yield :content_type, 'text/plain'
      end
    end.new
    closing_body = Class.new do
      def initialize(chunks)
        @chunks = chunks
      end

      def each(&block)
        @chunks.each(&block)
      end

      def close
        @closed = true
      end

      def closed?
        @closed == true
      end
    end.new(%w[O K])

    described_class.install!(lambda { |env|
      captured_env = env
      [200, headers, closing_body]
    })

    status, headers, body = described_class.call(
      [
        ['REQUEST_METHOD', 'GET'],
        ['SCRIPT_NAME', ''],
        ['PATH_INFO', '/projects'],
        ['QUERY_STRING', 'filter=active'],
        ['rack.url_scheme', 'http']
      ]
    )

    expect(status).to eq(200)
    expect(headers).to eq([['content_type', 'text/plain']])
    expect(body).to eq('OK')
    expect(closing_body.closed?).to be(true)
    expect(captured_env).to include(
      'REQUEST_METHOD' => 'GET',
      'SCRIPT_NAME' => '',
      'PATH_INFO' => '/projects',
      'QUERY_STRING' => 'filter=active',
      'rack.url_scheme' => 'http',
      'rack.multithread' => false,
      'rack.multiprocess' => false,
      'rack.run_once' => false
    )
    expect(captured_env.fetch('rack.input').read).to eq('')
    expect(captured_env.fetch('rack.errors')).to equal($stderr)
  end

  it 'returns nil when no Rack app is installed' do
    expect(described_class.call([%w[REQUEST_METHOD GET]])).to be_nil
  end

  it 'collects a body that does not implement close' do
    described_class.install!(->(_env) { [204, {}, []] })

    status, headers, body = described_class.call([%w[REQUEST_METHOD HEAD]])

    expect(status).to eq(204)
    expect(headers).to eq([])
    expect(body).to eq('')
  end

  it 'preserves binary body bytes' do
    described_class.install!(->(_env) { [200, {}, ["a\0b".b]] })

    _status, _headers, body = described_class.call([%w[REQUEST_METHOD GET]])

    expect(body.bytes).to eq([97, 0, 98])
  end
end
