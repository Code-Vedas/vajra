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

  it 'accepts callable rack app objects' do
    app = Class.new do
      def call(_env)
        [200, { 'Content-Type' => 'text/plain' }, ['OK']]
      end
    end.new

    expect(described_class.install!(app)).to equal(app)
  end

  it 'rejects non-callable rack app objects' do
    expect { described_class.install!(Object.new) }
      .to raise_error(TypeError, 'Rack app must respond to #call')
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
      ],
      "abc\x00".b
    )

    expect(status).to eq(200)
    expect(headers).to eq([['content-type', 'text/plain']])
    expect(body).to eq('OK')
    expect(closing_body.closed?).to be(true)
    expect(captured_env).to include(
      'REQUEST_METHOD' => 'GET',
      'SCRIPT_NAME' => '',
      'PATH_INFO' => '/projects',
      'QUERY_STRING' => 'filter=active',
      'rack.version' => [1, 6],
      'rack.url_scheme' => 'http',
      'rack.multithread' => false,
      'rack.multiprocess' => false,
      'rack.run_once' => false
    )
    expect(captured_env.fetch('rack.input').external_encoding).to eq(Encoding::BINARY)
    expect(captured_env.fetch('rack.input').read(2)).to eq('ab')
    expect(captured_env.fetch('rack.input').read).to eq("c\x00".b)
    captured_env.fetch('rack.input').rewind
    expect(captured_env.fetch('rack.input').read).to eq("abc\x00".b)
    expect(captured_env.fetch('rack.errors')).to equal($stderr)
  end

  it 'returns nil when no Rack app is installed' do
    expect(described_class.call([%w[REQUEST_METHOD GET]], ''.b)).to be_nil
  end

  it 'collects a body that does not implement close' do
    described_class.install!(->(_env) { [204, {}, []] })

    status, headers, body = described_class.call([%w[REQUEST_METHOD HEAD]], ''.b)

    expect(status).to eq(204)
    expect(headers).to eq([])
    expect(body).to eq('')
  end

  it 'preserves binary body bytes' do
    described_class.install!(->(_env) { [200, {}, ["\xFF\0\x80".b]] })

    _status, _headers, body = described_class.call([%w[REQUEST_METHOD GET]], ''.b)

    expect(body.encoding).to eq(Encoding::BINARY)
    expect(body.bytes).to eq([255, 0, 128])
  end

  it 'does not swallow close errors from the response body' do
    closing_body = Class.new do
      def each
        yield 'OK'
      end

      def close
        nil.missing_method
      end
    end.new

    described_class.install!(->(_env) { [200, {}, closing_body] })

    expect { described_class.call([%w[REQUEST_METHOD GET]], ''.b) }.to raise_error(NoMethodError)
  end

  it 'closes the response body when status normalization raises' do
    closing_body = Class.new do
      def each
        yield 'OK'
      end

      def close
        @closed = true
      end

      def closed?
        @closed == true
      end
    end.new

    described_class.install!(->(_env) { ['bad-status', {}, closing_body] })

    expect { described_class.call([%w[REQUEST_METHOD GET]], ''.b) }.to raise_error(ArgumentError)
    expect(closing_body.closed?).to be(true)
  end

  it 'closes the response body exactly once when body collection raises' do
    closing_body = Class.new do
      def initialize
        @close_calls = 0
      end

      attr_reader :close_calls

      def each
        raise 'body exploded'
      end

      def close
        @close_calls += 1
      end
    end.new

    described_class.install!(->(_env) { [200, {}, closing_body] })

    expect { described_class.call([%w[REQUEST_METHOD GET]], ''.b) }.to raise_error(RuntimeError, 'body exploded')
    expect(closing_body.close_calls).to eq(1)
  end
end
